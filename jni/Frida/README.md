# Frida GUM Libraries

Place your frida-gum static library and headers here:

```
Frida/
  arm64-v8a/
    libfrida-gum.a    <- frida-gum for arm64
    frida-gum.h       <- frida-gum header
  armeabi-v7a/
    libfrida-gum.a    <- frida-gum for armeabi-v7a (if needed)
    frida-gum.h
  gumpp/              <- gumpp C++ wrapper (already present)
```

If libfrida-gum.a is not found, CMake will auto-disable Frida (USE_FRIDA=OFF).
