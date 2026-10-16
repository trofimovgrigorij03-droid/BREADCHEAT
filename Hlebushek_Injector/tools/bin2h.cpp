// bin2h: превращает файл в C-массив (DLL_DATA / DLL_SIZE), чтобы вшить DLL внутрь exe.
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bin2h in out\n"); return 1; }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 1;
    std::fprintf(out, "static const unsigned char DLL_DATA[] = {");
    unsigned long n = 0;
    int c;
    while ((c = std::fgetc(in)) != EOF) {
        if (n % 32 == 0) std::fprintf(out, "\n");
        std::fprintf(out, "%d,", c);
        n++;
    }
    std::fprintf(out, "0};\nstatic const unsigned long DLL_SIZE = %lu;\n", n);
    std::fclose(in);
    std::fclose(out);
    return 0;
}
