#include "FileDescriptorInput.hpp"
#include "BlockQueue.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>
#include <cmath>
#include <fstream>
#include <cstddef>
#include <iterator>
#include <cstdint>
#if !defined(NO_TIFF)
#include <stdio.h>
#include <tiffio.h>
#endif
#if !defined(NO_PNG)
#include <memory>
#include <csetjmp>
#include <png.h>
#endif

typedef std::vector<std::vector<std::vector<float>>> Image;
#define READIMAGEOUT_TYPE ReadImageOutTemplate<Image,std::string>

#include "readimage_io.hpp"


int read_whole_file(std::vector<std::byte>& Contents, const char* Filename) {
    int fd = open(Filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;
    struct stat info;
    if (-1 == fstat(fd, &info)) {
        close(fd);
        return -2;
    }
    Contents.resize(info.st_size);
    int got = read(fd, &Contents.front(), info.st_size);
    close(fd);
    return Contents.size() - got;
}


typedef const char* (*ReadFunc)(const ReadImageInValues::filenameType&, Image&);

#if !defined(NO_TIFF)
static std::string tiff_error;

static void handle_tiff_error(const char* module, const char* fmt, va_list ap) {
    std::vector<char> buffer;
    buffer.resize(256);
    tiff_error = module;
    tiff_error += ": ";
retry:
    int status = vsnprintf(&buffer.front(), buffer.size(), fmt, ap);
    if (buffer.size() <= status) {
        buffer.resize(status + 1);
        goto retry;
    }
    if (status < 0)
        tiff_error += "Failed to print.";
    else
        tiff_error += &buffer.front();
}

static int read_tiff(
    const ReadImageInValues::filenameType& filename, Image& image)
{
    TIFFSetWarningHandler(NULL);
    TIFFSetErrorHandler(&handle_tiff_error);
    TIFF* t = TIFFOpen(filename.c_str(), "r");
    if (t == nullptr)
        return -1;
    uint16 bits, samples;
    uint32 width, height;
    TIFFGetField(t, TIFFTAG_BITSPERSAMPLE, &bits);
    if (bits != 8 && bits != 16) {
        TIFFClose(t);
        return -2;
    }
    TIFFGetField(t, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &height);
    if (samples != 1) {
        uint16 config;
        TIFFGetField(t, TIFFTAG_PLANARCONFIG, &config);
        if (config != PLANARCONFIG_CONTIG) {
            TIFFClose(t);
            return -3;
        }
    }
    std::unique_ptr<void,void (*)(void*)> buffer(
        _TIFFmalloc(TIFFScanlineSize(t)), &_TIFFfree);
    image.resize(height);
    uint32 row = 0;
    for (auto& line : image) {
        line.resize(width);
        if (-1 == TIFFReadScanline(t, buffer.get(), row++))
            return -4;
        unsigned char* curr = reinterpret_cast<unsigned char*>(buffer.get());
        for (auto& pixel : line) {
            pixel.resize(samples);
            for (auto& component : pixel)
                if (bits == 8)
                    component = float(*curr++);
                else {
                    component = float(*curr) * 256.0f + float(curr[1]);
                    curr += 2;
                }
        }
    }
    TIFFClose(t);
    return 0;
}

static const char* readTIFF(
    const ReadImageInValues::filenameType& filename, Image& image)
{
    int status = read_tiff(filename, image);
    switch (status) {
    case 0: return nullptr;
    case -1: return "Failed to open file.";
    case -2: return "Unsupported bit depth.";
    case -3: return "Not contiguous planar configuration.";
    case -4: return tiff_error.c_str();
    }
    return "Unspecified error.";
}
#endif

#if !defined(NO_PNG)
static void png_error_handler(png_structp unused, const char* error) {
    throw error;
}

static void png_warning_handler(png_structp unused, const char* unused2) { }

typedef void (*png_destroyer)(png_structp);
static void destroy_png(png_structp p) {
    png_destroy_read_struct(&p, nullptr, nullptr);
}

typedef void (*info_destroyer)(png_infop);
static png_structp png_s = nullptr;
static void destroy_info(png_infop p) {
    png_destroy_info_struct(png_s, &p);
}

static std::string png_error_message;

static void info_relay(png_structp png, png_infop info);
static void row_relay(png_structp png, png_bytep buffer,
    png_uint_32 row, int pass);
static void end_relay(png_structp png, png_infop info);

class ReadPNG {
private:
    const ReadImageInValues::filenameType& filename;
    Image& image;
    std::vector<std::byte> contents;
    png_uint_32 width, height;
    int passes, channels, bytes;
    std::vector<std::unique_ptr<png_byte>> raw;

    int read() {
        int status = read_whole_file(contents, filename.c_str());
        if (status != 0)
            return status;
        std::unique_ptr<png_struct,png_destroyer> png(
            png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                &png_error_handler, &png_warning_handler),
            &destroy_png);
        png_s = png.get();
        std::unique_ptr<png_info,info_destroyer> info(
            png_create_info_struct(png.get()), &destroy_info);
        png_set_progressive_read_fn(
            png.get(), this, &info_relay, &row_relay, &end_relay);
        if (setjmp(png_jmpbuf(png.get())))
            return -4;
        png_process_data(png.get(), info.get(),
            reinterpret_cast<png_bytep>(&contents.front()), contents.size());
        return 0;
    }

public:
    ReadPNG(const ReadImageInValues::filenameType& Filename, Image& I)
        : filename(Filename), image(I),
        width(0), height(0), passes(1), channels(0), bytes(0) { }

    int Read() {
        try {
            return read();
        }
        catch (const char* e) {
            png_error_message = e;
            return -3;
        }
        catch (const int e) {
            return e;
        }
    }

    void info_callback(png_structp png, png_infop info) {
        int bit_depth, color_type, interlace_type;
        png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type,
            &interlace_type, nullptr, nullptr);
        switch (color_type) {
        case PNG_COLOR_TYPE_GRAY:
            channels = 1;
            if (bit_depth < 8)
                png_set_expand_gray_1_2_4_to_8(png);
            if (png_get_valid(png, info, PNG_INFO_tRNS)) {
                png_set_tRNS_to_alpha(png);
                channels = 2;
            }
            break;
        case PNG_COLOR_TYPE_GRAY_ALPHA: channels = 2; break;
        case PNG_COLOR_TYPE_PALETTE:
            channels = 3;
            png_set_palette_to_rgb(png);
            if (png_get_valid(png, info, PNG_INFO_tRNS)) {
                png_set_tRNS_to_alpha(png);
                channels = 4;
            }
            break;
        case PNG_COLOR_TYPE_RGB:
            channels = 3;
            if (png_get_valid(png, info, PNG_INFO_tRNS)) {
                png_set_tRNS_to_alpha(png);
                channels = 4;
            }
            break;
        case PNG_COLOR_TYPE_RGB_ALPHA: channels = 4; break;
        default:
            throw -4;
        }
        if (interlace_type != PNG_INTERLACE_NONE)
            passes = png_set_interlace_handling(png);
        png_read_update_info(png, info);
        bytes = (8 < bit_depth) ? 2 : 1;
        for (png_uint_32 k = 0; k < height; k++)
            raw.push_back(std::unique_ptr<png_byte>(
                new png_byte[channels * width * bytes]));
    }

    void row_callback(png_structp png, png_bytep buffer,
        png_uint_32 row, int pass)
    {
        png_progressive_combine_row(png, raw[row].get(), buffer);
    }

    void end_callback(png_structp png, png_infop info) {
        image.resize(height);
        size_t k = 0;
        for (auto& line : image) {
            line.resize(width);
            png_bytep curr = raw[k].get();
            for (auto& pixel : line) {
                pixel.resize(channels);
                for (auto& component : pixel)
                    if (bytes == 1)
                        component = float(*curr++);
                    else {
                        component = (float(curr[0]) * 256.0f) + float(curr[1]);
                        curr += 2;
                    }
            }
            raw[k++].reset();
        }
    }
};

static void info_relay(png_structp png, png_infop info) {
    ReadPNG* p = reinterpret_cast<ReadPNG*>(png_get_progressive_ptr(png));
    p->info_callback(png, info);
}

static void row_relay(png_structp png, png_bytep buffer,
    png_uint_32 row, int pass)
{
    ReadPNG* p = reinterpret_cast<ReadPNG*>(png_get_progressive_ptr(png));
    p->row_callback(png, buffer, row, pass);
}

static void end_relay(png_structp png, png_infop info) {
    ReadPNG* p = reinterpret_cast<ReadPNG*>(png_get_progressive_ptr(png));
    p->end_callback(png, info);
}

static const char* readPNG(
    const ReadImageInValues::filenameType& filename, Image& image)
{
    ReadPNG reader(filename, image);
    int status = reader.Read();
    if (status > 0)
        return "Failed to read whole file.";
    switch (status) {
    case 0: return nullptr;
    case -1: return "Failed to open file.";
    case -2: return "Failed to get file size.";
    case -3: return png_error_message.c_str();
    case -4: return "Unrecognized color type.";
    }
    return "Unspecified error.";
}
#endif

// PPM, NetPBM color image binary format.

static int read_ppm(
    const ReadImageInValues::filenameType& filename, Image& image)
{
    std::vector<std::byte> contents;
    int status = read_whole_file(contents, filename.c_str());
    if (status != 0)
        return status;
    // Read P6 width height maximum
    if (contents.size() < 12)
        return -3;
    if (contents[0] != static_cast<std::byte>('P'))
        return -3;
    bool binary = contents[1] == static_cast<std::byte>('6');
    if (!binary && contents[1] != static_cast<std::byte>('3'))
        return -3;
    if (!binary)
        contents.push_back(std::byte(0));
    int width, height, maxval;
    const char* last = reinterpret_cast<const char*>(&contents.back());
    const char* curr = reinterpret_cast<const char*>(&contents.front() + 2);
    size_t idx;
    // Comment lines are not supported in the file.
    ParserPool pp;
    ParseInt p;
    try {
        curr = p.skipWhitespace(curr, last);
        curr = p.Scan(curr, last, pp);
        if (curr == nullptr || !p.isWhitespace(*curr))
            return -4;
        width = std::get<ParserPool::Int>(pp.Value);
        curr = p.skipWhitespace(curr, last);
        curr = p.Scan(curr, last, pp);
        if (curr == nullptr || !p.isWhitespace(*curr))
            return -4;
        height = std::get<ParserPool::Int>(pp.Value);
        curr = p.skipWhitespace(curr, last);
        curr = p.Scan(curr, last, pp);
        if (curr == nullptr || !p.isWhitespace(*curr))
            return -4;
        maxval = std::get<ParserPool::Int>(pp.Value);
        if (width <= 0 || height <= 0 || maxval <= 0 || 65535 < maxval)
            return -4;
        if (binary) {
            curr++; // Skip whitespace.
            idx = reinterpret_cast<const std::byte*>(curr) - &contents.front();
            if (contents.size() - idx != width * height * ((maxval < 256) ? 3 : 6))
                return -5;
        }
    }
    catch (const ParserException& e) {
        return -4;
    }
    image.resize(height);
    for (auto& line : image) {
        line.resize(width);
        for (auto& pixel : line) {
            pixel.resize(3);
            for (auto& component : pixel)
                if (binary) {
                    if (maxval < 256) {
                        component = float(contents[idx]);
                        ++idx;
                    } else {
                        component = float(contents[idx]) * 256 + float(contents[idx + 1]);
                        idx += 2;
                    }
                } else {
                    curr = p.skipWhitespace(curr, last);
                    if (curr == nullptr)
                        return -6;
                    curr = p.Scan(curr, last, pp);
                    if (curr == nullptr)
                        return -7;
                    component = std::get<ParserPool::Int>(pp.Value);
                }
        }
    }
    return 0;
}

static const char* readPPM(
    const ReadImageInValues::filenameType& filename, Image& image)
{
    int status = read_ppm(filename, image);
    if (status > 0)
        return "Failed to read whole file.";
    switch (status) {
    case 0: return nullptr;
    case -1: return "Failed to open file.";
    case -2: return "Failed to get file size.";
    case -3: return "Not PPM.";
    case -4: return "Invalid header.";
    case -5: return "File and header size mismatch.";
    case -6: return "No whitespace when expected.";
    case -7: return "No number when expected.";
    }
    return "Unspecified error.";
}

static void report(const ReadImageOut& Out) {
    std::vector<char> buffer;
    Write(std::cout, Out, buffer);
}

const size_t block_size = 4096; // Presumably way bigger than single input.

int main(int argc, char** argv) {
    int f = 0;
    if (argc > 1)
        f = open(argv[1], O_RDONLY);
    FileDescriptorInput input(f);
    BlockQueue::BlockPtr buffer;
    ParserPool pp;
    ReadImageIn parser;
    const char* end = nullptr;
    while (!input.Ended()) {
        if (end == nullptr) {
            if (!buffer)
                buffer.reset(new BlockQueue::Block());
            if (buffer->size() != block_size + 1)
                buffer->resize(block_size + 1);
            int count = input.Read(&buffer->front(), block_size);
            if (count == 0)
                continue;
            buffer->resize(count + 1);
            buffer->back() = 0;
            end = &buffer->front();
        }
        if (parser.Finished()) {
            end = pp.skipWhitespace(end, &buffer->back());
            if (end == nullptr)
                continue;
        }
        try {
            end = parser.Scan(end, &buffer->back(), pp);
        }
        catch (const ParserException& e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
        if (!parser.Finished()) {
            end = nullptr;
            continue;
        }
        ReadImageInValues val;
        parser.Swap(val.values);
        ReadImageOut out;
        if (!val.formatGiven()) {
            size_t last = val.filename().find_last_of(".");
            if (last == std::string::npos) {
                out.error = "No format nor extension in filename.";
                report(out);
                continue;
            }
            val.format() = val.filename().substr(last + 1);
        }
        ReadFunc reader = nullptr;
        float shift = 0.0f;
        float scale = 1.0f;
        if (val.minimumGiven()) {
            shift = val.minimum();
            if (val.maximumGiven()) {
                if (val.maximum() <= val.minimum()) {
                    out.error = "maximum <= minimum";
                    report(out);
                    continue;
                }
                scale = val.maximum() - val.minimum();
            }
        } else if (val.maximumGiven())
            shift = val.maximum();
        if (strcasecmp(val.format().c_str(), "ppm") == 0 ||
            strcasecmp(val.format().c_str(), "p6-ppm") == 0 ||
            strcasecmp(val.format().c_str(), "p3-ppm") == 0)
                reader = &readPPM;
#if !defined(NO_TIFF)
        else if (strcasecmp(val.format().c_str(), "tiff") == 0 ||
            strcasecmp(val.format().c_str(), "tif") == 0)
                reader = &readTIFF;
#endif
#if !defined(NO_PNG)
        else if (strcasecmp(val.format().c_str(), "png") == 0)
            reader = &readPNG;
#endif
        else {
            out.error = "Unsupported format: " + val.format();
            report(out);
            continue;
        }
        const char* err = reader(val.filename(), out.image);
        if (err) {
            out.error = std::string(err);
            report(out);
            continue;
        }
        // Data is positive integers at this point.
        float minval, maxval;
        minval = maxval = out.image[0][0][0];
        for (auto& line : out.image)
            for (auto& pixel : line)
                for (auto& component : pixel) {
                    if (component < minval)
                        minval = component;
                    if (maxval < component)
                        maxval = component;
                }
        maxval += 1;
        // The 0.25 is to ensure that if values were truncated (writeimage)
        // then the number is not at the edge of the range, which, if written
        // and read repeatedly, risks moving values towards zero. If rounding
        // is used, then original integer is at the middle but 0.25 still keeps
        // the number from changing to a higher value (0.5 would cause that).
        if (val.minimumGiven() || val.maximumGiven())
            shift += 0.25f - minval;
        if (val.minimumGiven() && val.maximumGiven())
            scale /= (maxval - minval);
        for (auto& line : out.image)
            for (auto& pixel : line)
                for (auto& component : pixel)
                    component = (component + shift) * scale;
        report(out);
    }
    return 0;
}
