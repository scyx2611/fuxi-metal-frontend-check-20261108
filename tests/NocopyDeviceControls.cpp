// Synthetic-only iOS control; this does not inspect QuartzCore or texture bytes.
#define main runUnwindControl
#include "UnwindRegisterTests.cpp"
#undef main
#define main runSafeReadControl
#include "NocopySafeReadTests.cpp"
#undef main
int main() {
 if(runUnwindControl()!=0 || runSafeReadControl()!=0)return 1;
 std::puts("FUXI PROVENANCE CONTROL PASS");
 return 0;
}
