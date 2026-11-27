# Build the RGBA terrain generator

The experiment is built from the repository root so it can use the shared
ws::image library and the workspace's pinned stb_image dependency.

## Windows PowerShell

From the repository root:

    .\experiments\rgba_terrain_poc\build.ps1

For a clean Release build:

    .\experiments\rgba_terrain_poc\build.ps1 -Clean -Configuration Release

The script configures only the core + image libraries, disables applications,
builds the terrain generator and both test targets, then runs the image and
terrain tests. On the first run CMake FetchContent needs network access to
obtain the repository's pinned third-party dependencies.

## Manual CMake equivalent

Run these commands from the repository root:

    cmake -S . -B build/rgba-terrain-poc -DWS_BUILD_APPS=OFF -DWS_BUILD_EXPERIMENTS=ON -DWS_BUILD_TESTS=ON -DWS_BUILD_LIBS=image
    cmake --build build/rgba-terrain-poc --config Release --target rgba_terrain_poc rgba_terrain_tests test_image --parallel
    ctest --test-dir build/rgba-terrain-poc -C Release --output-on-failure -R "^(image($|[.])|rgba_terrain_tests$)"

The executable is written to build/rgba-terrain-poc/bin. Invoke it with:

    rgba_terrain_poc input.png input.rgba-terrain output.voxcols

The input must be an 8-bit RGBA PNG. The loader verifies that the encoded
source has four channels; it does not silently convert RGB or grayscale
images into RGBA data maps. Pixel coordinates and channel byte values are
preserved in row-major order.
