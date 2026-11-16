void log_line(const char* fmt, ...) {
    FILE* fp = std::fopen(kLog, "a");