/* Second shared library: proves multi-DT_NEEDED loading + cross-library symbol
   resolution.  Constructors are deliberately not used (the loader's DT_INIT
   support is not stable yet). */
int extra_answer(void) { return 42; }
