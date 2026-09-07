// Async core lifecycle tests, rebuilt from the current implementation.
// Each test pins one surviving invariant of the Completion FSM and the
// RequestArena slot lifecycle as they behave in the live submit_transaction
// path. Run via the sluice_async_core_lifecycle target; optional argv[1]
// filters tests by substring.

#include "async_test_kit.hpp"

int main(int argc, char** argv) {
    return ::sluice_test::run_all(argc, argv);
}
