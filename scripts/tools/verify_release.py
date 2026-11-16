#!/usr/bin/env python3
"""Static checks for the combined PrefBar PS5 release payload."""

from __future__ import annotations

import os
import pathlib
import struct
import sys


def enabled(name: str, default: str = "ON") -> bool:
    return os.environ.get(name, default).upper() in {"ON", "1", "TRUE", "YES"}


def renderer_elf_issues(elf: bytes) -> list[str]:
    """Validate the ELF features consumed by PrefBar's in-tree loader."""
    issues: list[str] = []
    elf_header = struct.Struct("<16sHHIQQQIHHHHHH")
    program_header = struct.Struct("<IIQQQQQQ")
    section_header = struct.Struct("<IIQQQQIIQQ")
    dynamic_entry = struct.Struct("<qQ")
    rela_entry = struct.Struct("<QQq")
    symbol_entry = struct.Struct("<IBBHQQ")

    if len(elf) < elf_header.size:
        return ["ELF header is truncated"]

    fields = elf_header.unpack_from(elf)
    ident, elf_type, machine, version = fields[:4]
    entry, phoff, shoff = fields[4:7]
    ehsize, phentsize, phnum, shentsize, shnum = fields[8:13]

    if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1 or ident[6] != 1:
        issues.append("expected little-endian ELF64")
    if elf_type != 3:
        issues.append(f"expected ET_DYN renderer (e_type={elf_type})")
    if machine != 62:
        issues.append(f"expected EM_X86_64 renderer (e_machine={machine})")
    if version != 1 or ehsize != elf_header.size:
        issues.append("invalid ELF header version or size")

    if phentsize != program_header.size or phnum == 0:
        issues.append("program-header table has an unsupported entry size or is empty")
        return issues
    if phoff > len(elf) or phnum > (len(elf) - phoff) // program_header.size:
        issues.append("program-header table extends beyond the ELF")
        return issues

    have_load = False
    entry_executable = False
    have_dynamic = False
    for index in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _align = (
            program_header.unpack_from(elf, phoff + index * program_header.size)
        )
        if p_offset > len(elf) or p_filesz > len(elf) - p_offset:
            issues.append(f"program segment {index} extends beyond the ELF")
            continue
        if p_type == 3:  # PT_INTERP
            issues.append("PT_INTERP is unsupported by the injected loader")
        elif p_type == 1:  # PT_LOAD
            have_load = True
            if p_filesz > p_memsz:
                issues.append(f"PT_LOAD segment {index} has p_filesz > p_memsz")
            if (p_flags & 1) and entry >= p_vaddr and entry - p_vaddr < p_memsz:
                entry_executable = True
        elif p_type == 2:  # PT_DYNAMIC
            have_dynamic = True
            if p_filesz % dynamic_entry.size:
                issues.append("PT_DYNAMIC size is not a multiple of Elf64_Dyn")
                continue
            for offset in range(p_offset, p_offset + p_filesz, dynamic_entry.size):
                tag, value = dynamic_entry.unpack_from(elf, offset)
                if tag == 0:  # DT_NULL
                    break
                if tag == 0x6FFFFFFB and (value & 0x08000000):  # DT_FLAGS_1 / DF_1_PIE
                    issues.append("DF_1_PIE is unsupported by the injected loader")

    if not have_load:
        issues.append("renderer has no PT_LOAD segment")
    if not entry_executable:
        issues.append("ELF entry point is not inside an executable PT_LOAD segment")
    if not have_dynamic:
        issues.append("renderer has no PT_DYNAMIC segment")

    if shoff == 0 or shentsize != section_header.size or shnum == 0:
        issues.append("section headers are missing or use an unsupported entry size")
        return issues
    if shoff > len(elf) or shnum > (len(elf) - shoff) // section_header.size:
        issues.append("section-header table extends beyond the ELF")
        return issues

    sections = [
        section_header.unpack_from(elf, shoff + index * section_header.size)
        for index in range(shnum)
    ]
    for index, section in enumerate(sections):
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = (
            section[1], section[4], section[5], section[6], section[9]
        )
        if sh_type != 4:  # SHT_RELA
            continue
        if sh_entsize != rela_entry.size or sh_size % rela_entry.size:
            issues.append(f"SHT_RELA section {index} has an unsupported entry size")
            continue
        if sh_offset > len(elf) or sh_size > len(elf) - sh_offset:
            issues.append(f"SHT_RELA section {index} extends beyond the ELF")
            continue

        symbol_section = None
        string_section = None
        if sh_link < len(sections):
            candidate = sections[sh_link]
            if candidate[1] in (2, 11) and candidate[9] == symbol_entry.size:
                if candidate[5] % symbol_entry.size == 0 and candidate[6] < len(sections):
                    strings = sections[candidate[6]]
                    if strings[1] == 3 and strings[4] <= len(elf) and strings[5] <= len(elf) - strings[4]:
                        symbol_section = candidate
                        string_section = strings

        for rela_offset in range(sh_offset, sh_offset + sh_size, rela_entry.size):
            _target, info, _addend = rela_entry.unpack_from(elf, rela_offset)
            reloc_type = info & 0xFFFFFFFF
            symbol_index = info >> 32
            if reloc_type not in (0, 1, 6, 8):  # NONE, 64, GLOB_DAT, RELATIVE
                issues.append(f"unsupported x86-64 relocation type {reloc_type} in SHT_RELA {index}")
                break
            if reloc_type in (1, 6):
                if symbol_section is None or string_section is None:
                    issues.append(f"symbol relocation in SHT_RELA {index} has no valid symbol/string table")
                    break
                symbol_count = symbol_section[5] // symbol_entry.size
                if symbol_index >= symbol_count:
                    issues.append(f"symbol index {symbol_index} is out of range in SHT_RELA {index}")
                    break
                sym_offset = symbol_section[4] + symbol_index * symbol_entry.size
                if sym_offset > len(elf) or symbol_entry.size > len(elf) - sym_offset:
                    issues.append(f"symbol {symbol_index} extends beyond the ELF")
                    break
                name_offset, _info, _other, shndx, _value, _size = symbol_entry.unpack_from(elf, sym_offset)
                if name_offset >= string_section[5]:
                    issues.append(f"symbol name offset is out of range in SHT_RELA {index}")
                    break
                name_start = string_section[4] + name_offset
                name_end = elf.find(b"\0", name_start, string_section[4] + string_section[5])
                if name_end < 0:
                    issues.append(f"symbol name is unterminated in SHT_RELA {index}")
                    break
                if shndx == 0 and name_end == name_start:
                    issues.append(f"undefined symbol {symbol_index} has an empty name")
                    break

    return issues


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: verify_release.py controller.elf shellui_renderer.elf",
            file=sys.stderr,
        )
        return 2

    controller_path, renderer_path = map(pathlib.Path, sys.argv[1:3])
    try:
        controller = controller_path.read_bytes()
        renderer = renderer_path.read_bytes()
    except OSError as exc:
        print(f"ERROR: could not read release artifact: {exc}", file=sys.stderr)
        return 2

    elf_issues = renderer_elf_issues(renderer)
    checks = [
        (not elf_issues, "renderer ELF is incompatible with PrefBar's loader: " + "; ".join(elf_issues)),
        (controller.startswith(b"\x7fELF"), "controller is not an ELF"),
        (renderer.startswith(b"\x7fELF"), "ShellUI renderer is not an ELF"),
        (len(renderer) > 4096, "ShellUI renderer ELF is unexpectedly small"),
        (controller.find(renderer) > 0, "exact renderer ELF is not embedded in controller"),
        (b"PrefBar V0.3.0 controller" in controller, "PrefBar controller identity marker missing"),
        (b"/data/prefbar" in controller, "PrefBar runtime data path missing"),
        (b"game.detected" in controller, "game-process lifecycle marker missing"),
        (b"renderer.online" in controller, "renderer lifecycle marker missing"),
        (b"process.name" in controller and b"main thread name request" in controller,
         "PrefBar main-thread naming request marker missing"),
        (b"/data/prefbar/prefbar.conf" in controller,
         "PrefBar runtime configuration path missing"),
        (b"read_raw_rc=%d" in controller and b"read_attempted=%d" in controller and b"read_reported_len=%u" in controller and b"read_requested=%u" in controller,
         "expanded hook-probe diagnostic markers missing"),
        (b"text_color=#F2F2F2" in controller and b"text_shadow=1" in controller,
         "default PrefBar appearance configuration missing"),
        (b"id_prefbar_shadow" in renderer,
         "optional text-shadow renderer path missing"),
        (b"FAN %d%%" in controller and b"fan_duty=%d\n" in controller,
         "fan-duty overlay/status fields missing"),
        (b"prefbar: rows built" in renderer, "PrefBar telemetry row renderer marker missing"),
        (b"PBCTL1:OVERLAY:ON" in renderer and b"PBCTL1:OVERLAY:OFF" in renderer,
         "touchpad overlay control channel missing"),
        (b"file:///data" not in renderer,
         "renderer contains a file:///data reference"),
        (b"cxml://psnotification" not in renderer,
         "renderer must not send system notifications"),
        ((b"cxml://psnotification" in controller) == enabled("PREFBAR_DEBUG_NOTIFY"),
         "debug notification code does not match PREFBAR_DEBUG_NOTIFY"),
        ((b"PrefBar v%s made by X1NONs" in controller) == enabled("PREFBAR_DEBUG_NOTIFY"),
         "startup attribution toast does not match PREFBAR_DEBUG_NOTIFY"),
    ]

    failed = [message for ok, message in checks if not ok]
    for message in failed:
        print(f"ERROR: {message}", file=sys.stderr)
    if failed:
        return 1

    print("PrefBar release artifact checks: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
