// pse_tests.exe: runs every PSE_TEST. Argument = substring filter. Exit code = failed tests (max 255).
#include <windows.h>

#include "pse_test.h"

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedTests = 0, skippedTests = 0;
    for (const auto& c : pse_test::Registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        ++run;
        int before = pse_test::Failures();
        std::printf("[ RUN  ] %s\n", c.name);
        bool crashed = false, skipped = false;
        try {
            c.fn();
        } catch (const pse_test::Skipped& s) {
            skipped = true;
            std::printf("    SKIP %s\n", s.why.c_str());
        } catch (const pse_test::RequireFailed&) {
        } catch (...) {
            pse_test::Fail(c.file, c.line, "exception");
            crashed = true;
        }
        bool ok = pse_test::Failures() == before && !crashed;
        if (!ok) ++failedTests;
        else if (skipped) ++skippedTests;
        std::printf("[ %s ] %s\n", !ok ? "FAIL" : skipped ? "SKIP" : " OK ", c.name);
    }
    std::printf("\n%d test(s), %d failed, %d skipped\n", run, failedTests, skippedTests);
    return failedTests > 255 ? 255 : failedTests;
}
