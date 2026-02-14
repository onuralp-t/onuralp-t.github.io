#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float  f32;
typedef double f64;

bool file_read(u8** file_buffer, u64 *file_size, const char* source_filename)
{
    if (!source_filename || !file_buffer) {return false;}
    
    // FILE_OPEN
    FILE *f = fopen(source_filename, "rb");
    if (!f) {return false; }

    fseek(f, 0, SEEK_END);
    *file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // MEMORY_ALLOCATION
    *file_buffer = (u8*)malloc(*file_size + 1);

    if (!(*file_buffer)) {return false;}

    *(*file_buffer + *file_size) = '\0';

    fread(*file_buffer, sizeof(u8), *file_size, f);

    fclose(f);
    return true;
}

bool file_write(const char *destination_filename, u8* file_buffer, u64 file_size)
{
    if (!destination_filename || !file_buffer) {return false;}

    // FILE_OPEN
    FILE *f = fopen(destination_filename, "wb");
    if (!f) {return false;}

    fwrite(file_buffer, sizeof(u8), file_size, f);
    
    fclose(f);
    return true;
}

int main(void)
{
    u64 file_size;
    u8* file_buffer;
    file_read(&file_buffer, &file_size, "test.md");
    file_write("test.html", file_buffer, file_size);
    
    return 0;
}