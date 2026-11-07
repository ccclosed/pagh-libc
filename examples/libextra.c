/* Second shared library: proves multi-DT_NEEDED loading, cross-library symbol
   resolution and DT_INIT_ARRAY constructors. */
static int initialised;
__attribute__((constructor)) static void extra_init(void) { initialised = 7; }
int extra_answer(void) { return initialised * 6; }
