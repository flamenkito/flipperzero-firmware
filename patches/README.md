# Submodule patches

The firmware build compiles `lib/stm32wb_copro` in place, and Pocket AirBridge
production hardening required bounded SHCI/HCI command waits inside that library.

- `stm32wb_copro-bounded-command-wait.patch` — applied inside the submodule as
  local commit `9497127` on top of the v1.20.0 checkout (`133182d`).

A fresh clone cannot fetch that local commit. To reproduce:

```sh
git submodule update --init lib/stm32wb_copro
git -C lib/stm32wb_copro checkout 133182d
git -C lib/stm32wb_copro am ../patches/stm32wb_copro-bounded-command-wait.patch
```

Never run `git submodule update` blindly after this — it will not clobber a
committed HEAD, but it will detach/reset if the recorded gitlink is not found.
