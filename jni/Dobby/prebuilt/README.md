# Dobby Prebuilt

Place your compiled Dobby static libraries here:

```
Dobby/prebuilt/
  arm64-v8a/
    libdobby.a       <- your arm64 build
  armeabi-v7a/
    libdobby.a       <- your armeabi-v7a build (if needed)
```

The CMakeLists.txt will automatically detect these files.
If not found, it will try to build Dobby from source.

Header: Dobby/include/dobby.h (already present)
