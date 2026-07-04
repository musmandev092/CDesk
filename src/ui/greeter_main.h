/* greeter_main.h — `dankc greeter` subcommand entry point (docs/28-GREETER-
 * PLAN.md T4).
 *
 * Declares the reduced-init mode main.c dispatches into for `dankc greeter`
 * (see greeter_main.c for what "reduced" means and why). Kept as its own
 * header, separate from greeter.h (the UI surface module itself), so main.c's
 * one-line dispatch doesn't need to pull in greeter.h's dc_greeter type.
 */
#ifndef DC_UI_GREETER_MAIN_H
#define DC_UI_GREETER_MAIN_H

/* Runs the greeter to completion (or until interrupted) and does not return
 * in the normal case: on a successful greetd hand-off it _exit(0)s directly
 * (see greeter_main.c) so greetd can proceed to start the chosen session. It
 * only returns (with a non-zero status) for argument/environment failures
 * that occur before the event loop starts (e.g. no Wayland display, no
 * $GREETD_SOCK and not in demo mode). `argc`/`argv` are the full process
 * argv (argv[0] is "dankc", argv[1] is "greeter") — currently unused beyond
 * that dispatch, kept for parity with main() and future flag parsing. */
int dc_greeter_main(int argc, char **argv);

#endif /* DC_UI_GREETER_MAIN_H */
