<!-- Thanks for contributing! Read CONTRIBUTING.md for setup and testing.
     Keep each PR focused, and tell us what you tested. -->

## Problem

<!-- What was wrong or missing from the user's point of view?
     Link an issue if there is one. -->

## What changed

<!-- Describe the resulting behavior in a few sentences.
     For UI changes, include a screenshot or short video when useful. -->

## How it was tested

<!-- Describe what you did and what happened.
     For console tests, include firmware, runtime kit, and game/App ID
     where relevant. Attach logs for launch or runtime changes.

     Host tests and PS5 builds are useful, but aren't console testing.
     If you couldn't test on a console, leave that box unchecked
     and say what still needs checking. Mark unrelated checks N/A.

     For submodule pin changes, try a fresh recursive checkout and build
     to check that the pinned commits and required inputs are available. -->

- [ ] Host tests pass: `ctest --preset homebrew-host --output-on-failure`
- [ ] PS5 package builds: `scripts/build-homebrew-ps5.sh`
- [ ] Tested on a PS5, with steps and results described above
- [ ] Added or updated tests for changed behavior, where applicable

## Submodule changes

<!-- Delete this section if no submodule pins changed.
     For each changed pin, explain why and link the commits or comparison.
     Link an upstream PR when the change is useful upstream. -->

- [ ] Submodule changes are committed and pushed before this PR,
      so the pinned commits are fetchable from the configured remotes
- [ ] Z3CloudPlay's submodule pointers and corresponding revision records
      are updated together, including `dependencies.lock.json`
- [ ] For ps5-homebrew-ui, the revisions in `scripts/build-homebrew.sh`
      and `cmake/HomebrewUI.cmake` are also updated

## AI tools

<!-- Did you use an AI coding tool? Which one, with which model?
     Writing all the code with AI is a perfectly fine answer.
     Either way, you've read and run the result yourself. -->
