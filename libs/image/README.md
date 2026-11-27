# ws::image

A small reusable image I/O library. It wraps the repository's pinned
stb_image and stb_image_write dependencies behind workspace-owned types and
error reporting.

Example use:

    #include <ws/image/image.hpp>

    auto image = ws::image::load_rgba8_file("height-data.png");
    if (!image) {
        // image.error().message
        return;
    }
    // image->pixels is tightly packed RGBA8, top-to-bottom.

The write_rgba8_png_file function writes exact interleaved RGBA8 bytes to a
lossless PNG without color conversion or alpha premultiplication.

The generic load_file and load_memory functions can preserve the source channel
count or request 1, 2, 3, or 4 output channels. The strict RGBA entry points
also check that the encoded source itself had four channels; RGB/grayscale
images are not silently promoted into data maps.

Pixels are returned row-major, top-to-bottom and interleaved. The wrapper does
not resize, flip, color-grade, gamma-correct, or retain encoded input bytes.
The stb_image implementation lives in exactly one translation unit here.
Third-party headers remain private to the library's implementation.
