#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_timer.h>

typedef enum {
    WRAP,
    CLAMP,
} DisplayMode;

// Wrapping vs. clamping. Documentation is inconsistent on which one should be
// used. Some games require clamping to work (such as VBRIX) but others (like
// PONG) look better with wrap-around
static uint32_t displaymode = CLAMP;

// Hardware specs (RCA 1802 processor + a monochrome display at 60 Hz)
#define MEMORY_SIZE (4096) // Bytes, addressable with 6 bits (0x0fff)
#define REGISTER_COUNT (16)
#define STACK_SIZE (24) // Number of addresses (i.e. nested calls with chip-8)
#define OPCODE_SIZE (sizeof(uint16_t))
#define TIMER_RATE (60) // Hz
#define DISPLAY_WIDTH (64)
#define DISPLAY_HEIGHT (32)
#define FULLSCREEN (1)

// Memory layout
// 0-512 interpreter, 512-3744 program, 3744-3839 call stack internals etc,
// 3840-4095 display refresh 0x0-0x200,          0x200-0xea0, 0xea0-0xeff,
// 0xf00-0xfff
static uint8_t memory[MEMORY_SIZE]                     = {0};
static uint8_t registers[REGISTER_COUNT]               = {0};
static uint16_t stack[STACK_SIZE]                      = {0};
static uint8_t display[DISPLAY_WIDTH * DISPLAY_HEIGHT] = {0};

// 0x200 default pc, 0x600 for ETI 660 Chip-8 programs
static uint16_t pc               = 0x200;
static uint16_t address_register = 0x0;
static uint8_t stack_register    = 0;
static uint8_t delay_timer       = 0;
static uint8_t sound_timer       = 0;

// Control flow
static bool running = true;

#define ERROR(str)                                                             \
    {                                                                          \
        fflush(stdout);                                                        \
        fflush(stderr);                                                        \
        fprintf(stderr, "\n\nError in file %s line %d: %s\n", __FILE__,        \
                __LINE__, str);                                                \
        running = false;                                                       \
        exit(EXIT_FAILURE);                                                    \
    }

#define ERRCHK(retval)                                                         \
    {                                                                          \
        if (!(retval))                                                         \
            ERROR(#retval " was false");                                       \
    }

// Output
static SDL_Renderer* renderer   = NULL;
static SDL_Window* window       = NULL;
static SDL_Texture* framebuffer = NULL;
static int32_t color_bg         = 0x00000000; // 0xff5000ff;
static int32_t color_fg         = 0xffffffff;

static uint32_t
display_idx(const uint8_t i, const uint8_t j)
{
    ERRCHK(i < DISPLAY_WIDTH);
    ERRCHK(j < DISPLAY_HEIGHT);
    return i + j * DISPLAY_WIDTH;
}

static bool
eval_input(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            return false;
        }
        else if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_ESCAPE)
                return false;
        }
    }
    return true;
}

static bool
keydown(uint8_t code)
{
    const uint8_t* keystates = (uint8_t*)SDL_GetKeyboardState(NULL);

    const SDL_Scancode keys[] = {SDL_SCANCODE_X, SDL_SCANCODE_1, SDL_SCANCODE_2,
                                 SDL_SCANCODE_3, SDL_SCANCODE_Q, SDL_SCANCODE_W,
                                 SDL_SCANCODE_E, SDL_SCANCODE_A, SDL_SCANCODE_S,
                                 SDL_SCANCODE_D, SDL_SCANCODE_Z, SDL_SCANCODE_C,
                                 SDL_SCANCODE_4, SDL_SCANCODE_R, SDL_SCANCODE_F,
                                 SDL_SCANCODE_V};
    return keystates[keys[code]];
}

void
display_init(void)
{
    SDL_Init(SDL_INIT_EVERYTHING);

    const int display_scale = 20;
    window = SDL_CreateWindow("Chip-8", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              display_scale * DISPLAY_WIDTH,
                              display_scale * DISPLAY_HEIGHT, SDL_WINDOW_SHOWN);

    const uint32_t flags = SDL_RENDERER_TARGETTEXTURE;
    renderer             = SDL_CreateRenderer(window, -1, flags);

    if (FULLSCREEN)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

    framebuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH,
                                    DISPLAY_HEIGHT);
}

void
display_refresh(const uint8_t* display_contents)
{
    int32_t* pixels;
    int pitch;
    SDL_LockTexture(framebuffer, NULL, (void**)&pixels, &pitch);

    ERRCHK(pitch / sizeof(pixels[0]) == DISPLAY_WIDTH);
    for (int j = 0; j < DISPLAY_HEIGHT; ++j) {
        for (int i = 0; i < DISPLAY_WIDTH; ++i) {
            const size_t idx = i + j * pitch / sizeof(pixels[0]);
            pixels[idx] = display_contents[i + j * DISPLAY_WIDTH] ? color_fg
                                                                  : color_bg;
        }
    }

    SDL_UnlockTexture(framebuffer);
    SDL_RenderCopy(renderer, framebuffer, NULL, NULL);

    // Drawing done
    SDL_RenderPresent(renderer);
}

void
display_quit(void)
{
    SDL_DestroyTexture(framebuffer);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    framebuffer = NULL;
    renderer    = NULL;
    window      = NULL;
}

static void
load_sprites(void)
{
    const uint8_t sprites[] = {
        0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
        0x20, 0x60, 0x20, 0x20, 0x70, // 1
        0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
        0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
        0x90, 0x90, 0xF0, 0x10, 0x10, // 4
        0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
        0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
        0xF0, 0x10, 0x20, 0x40, 0x40, // 7
        0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
        0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
        0xF0, 0x90, 0xF0, 0x90, 0x90, // A
        0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
        0xF0, 0x80, 0x80, 0x80, 0xF0, // C
        0xE0, 0x90, 0x90, 0x90, 0xE0, // D
        0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
        0xF0, 0x80, 0xF0, 0x80, 0x80  // F
    };
    memcpy(&memory[0], sprites, sizeof(sprites));
}

static void
load_rom(const char* path)
{
    FILE* fp = fopen(path, "rb");
    ERRCHK(fp);

    const uint16_t pc_initial = pc;

    uint8_t byte;
    while (fread(&byte, sizeof(byte), 1, fp))
        memory[pc++] = byte;

    pc = pc_initial;

    fclose(fp);
}

static inline void
dump_memory(void)
{
    for (uint16_t i = 0; i < MEMORY_SIZE; ++i)
        printf("0x%.4x: 0x%.2x%s", i, memory[i], i % 5 ? "\t" : "\n");
    printf("\n\n");
}

static inline void
dump_registers(void)
{
    printf("PC: %u\n", pc);
    printf("Address register %u\n", address_register);
    printf("Stack register %u\n", stack_register);
    for (uint16_t i = 0; i < REGISTER_COUNT; ++i)
        printf("V%.1x: %u\n", i, registers[i]);
}

static inline uint16_t
convert_endianess(const uint16_t val)
{
    // Reverses byte order 0xAABB -> 0xBBAA
    return ((val & 0x00ff) << 8) | ((val & 0xff00) >> 8);
}

static inline void
dump_memory_opcodes(void)
{
    for (uint16_t i = 0; i < MEMORY_SIZE; i += sizeof(uint16_t))
        printf("0x%.4x: 0x%.4x%s", i,
               convert_endianess(*(uint16_t*)(&memory[i])),
               i / 2 % 4 ? "\t" : "\n");

    printf("\n\n");
}

static inline void
dump_rom(const char* path)
{
    FILE* fp = fopen(path, "rb");
    ERRCHK(fp);

    uint16_t opcode;
    while (fread(&opcode, sizeof(opcode), 1, fp))
        printf("0x%.4x ", convert_endianess(opcode));

    fclose(fp);
    fflush(stdout);
}

// Clear screen
// 00E0
static void
cls(void)
{
    printf("CLS");
    for (int j = 0; j < DISPLAY_HEIGHT; ++j)
        for (int i = 0; i < DISPLAY_WIDTH; ++i)
            display[display_idx(i, j)] = 0;
}

// 00EE
static void
ret(void)
{
    printf("return");
    ERRCHK(stack_register);

    --stack_register;
    pc = stack[stack_register];
}

// 0NNN
static void
chip8_syscall(const uint16_t addr)
{
    printf("syscall %u (NOT IMPLEMENTED)", addr);
    // NOP (the real machine would call an RCA 1802 program)
}

// 1NNN
static void
jmp(const uint16_t addr)
{
    printf("jmp %u (0x%.4x)", addr, addr);
    ERRCHK(pc - OPCODE_SIZE != addr);

    pc = addr;
}

// 2NNN
static void
call(const uint16_t addr)
{
    printf("call %u", addr);
    ERRCHK(stack_register < STACK_SIZE);

    stack[stack_register++] = pc;
    pc                      = addr;
}

// Skip if equal
// 3XNN
static void
se(const uint8_t vx, const uint8_t value)
{
    printf("se");
    if (registers[vx] == value)
        pc += OPCODE_SIZE;
}

// Skip if not equal
// 4XNN
static void
sne(const uint8_t vx, const uint8_t value)
{
    printf("sne");
    if (registers[vx] != value)
        pc += OPCODE_SIZE;
}

// Skip if equal registers
// 5XY0
static void
se_reg(const uint8_t vx, const uint8_t vy)
{
    printf("se_reg");
    if (registers[vx] == registers[vy])
        pc += OPCODE_SIZE;
}

// 6XNN
static void
ld(const uint8_t vx, const uint8_t val)
{
    printf("ld");
    registers[vx] = val;
}

// 7XNN
static void
add(const uint8_t vx, const uint8_t val)
{
    printf("add");
    registers[vx] = registers[vx] + val;
}

// 8XY0
static void
ld_reg(const uint8_t vx, const uint8_t vy)
{
    printf("ld_reg");
    registers[vx] = registers[vy];
}

// 8XY1
static void
or_reg(const uint8_t vx, const uint8_t vy)
{
    printf("or_reg");
    registers[vx] |= registers[vy];
}

// 8XY2
static void
and_reg(const uint8_t vx, const uint8_t vy)
{
    printf("and_reg");
    registers[vx] &= registers[vy];
}

// 8XY3
static void
xor_reg(const uint8_t vx, const uint8_t vy)
{
    printf("xor_reg");
    registers[vx] ^= registers[vy];
}

// 8XY4
static void
add_reg(const uint8_t vx, const uint8_t vy)
{
    printf("add_reg");
    registers[vx] += registers[vy];

    registers[0xf] = registers[vy] >
                     registers[vx]; // Set VF if there is a carry
}

// 8XY5
static void
sub_reg(const uint8_t vx, const uint8_t vy)
{
    printf("sub_reg");
    registers[0xf] = registers[vx] >
                     registers[vy]; // Set VF if there is a borrow
    registers[vx] -= registers[vy];
}

// 8XY6
static void
shr_reg(const uint8_t vx)
{
    printf("shr_reg");
    registers[0xf] = registers[vx] & 0x1; // Set VF to least significant bit
    registers[vx] >>= 1;
}

// 8XY7
static void
subn_reg(const uint8_t vx, const uint8_t vy)
{
    printf("subn_reg");
    registers[0xf] = registers[vy] >
                     registers[vx]; // Set VF if there is a borrow
    registers[vx] = registers[vy] - registers[vx];
}

// 8XYE
static void
shl_reg(const uint8_t vx)
{
    printf("shl_reg");
    registers[0xf] = registers[vx] >> 7; // VF to most significant bit
    registers[vx] <<= 1;
}

// Skip if not equal registers
// 9XY0
static void
sne_reg(const uint8_t vx, const uint8_t vy)
{
    printf("sne_reg");
    if (registers[vx] != registers[vy])
        pc += OPCODE_SIZE;
}

// ANNN
static void
ld_addr(const uint16_t addr)
{
    printf("ld_addr");
    address_register = addr;
}

// BNNN
static void
jmp_relative(const uint16_t offset)
{
    printf("jmp_relative");
    pc = registers[0x0] + offset;
}

// CXNN
static void
rnd(const uint8_t vx, const uint8_t mask)
{
    printf("rnd");
    registers[vx] = (uint8_t)(rand() % 255) & mask;
}

/*
static void
print_bin(uint8_t val)
{
    ERRCHK(sizeof(val) < sizeof(uint32_t));
    uint32_t bits    = sizeof(val) * 8 - 1;
    uint32_t bitmask = 0x1 << bits;
    do {
        printf("%s", (val & bitmask) > 0x0 ? "O" : ".");
        // printf("%u%s", (val & bitmask) > 0x0, bits & 0x7 ? "" : " ");
        bitmask >>= 1;
    } while (bits--);
    // printf("\n");
}*/

// DXYN
static void
draw(const uint8_t vx, const uint8_t vy, const uint8_t height)
{
    printf("draw");

    // Should delay to the start of a 60 Hz refresh
    // http://chip8.sourceforge.net/chip8-1.1.pdf

    const uint8_t i0 = registers[vx]; // & 0x3f;
    const uint8_t j0 = registers[vy]; // & 0x1f;

    registers[0xf] = 0x0;
    for (int j1 = 0; j1 < height; ++j1) {
        for (int i1 = 0; i1 < 8; ++i1) {
            if (memory[address_register + j1] & (0x80 >> i1)) {

                uint8_t i, j;
                if (displaymode == WRAP) {
                    i = (i0 + i1) % DISPLAY_WIDTH;
                    j = (j0 + j1) % DISPLAY_HEIGHT;
                }
                else { // CLAMP
                    i = (i0 + i1);
                    j = (j0 + j1);
                    if (i >= DISPLAY_WIDTH || j >= DISPLAY_HEIGHT)
                        break;
                }

                if (display[display_idx(i, j)])
                    registers[0xf] = 0x1; // Set VF if the pixel is already set

                display[display_idx(i, j)] ^= 1;
            }
        }
    }

    // Draw to stdout
    printf("\n\n");
    for (int j = 0; j < DISPLAY_HEIGHT; ++j) {
        for (int i = 0; i < DISPLAY_WIDTH; ++i) {
            printf("%s", display[display_idx(i, j)] ? "O" : ".");
        }
        printf("\n");
    }
}

// EX9E
static void
skip_p(const uint8_t vx)
{
    printf("skip_p");
    if (keydown(registers[vx]))
        pc += OPCODE_SIZE;
}

// EXA1
static void
skip_np(const uint8_t vx)
{
    printf("skip_np");
    if (!keydown(registers[vx]))
        pc += OPCODE_SIZE;
}

// FX0A
static void
getkey(const uint8_t vx)
{
    printf("getkey");
    for (uint8_t i = 0; i < 16; ++i) {
        if (keydown(i)) {
            registers[vx] = i;
            break;
        }

        // Block: execute the same instruction at next cycle
        if (i == 15)
            pc -= OPCODE_SIZE;
    }
}

// FX07
static void
ld_delay_to_reg(const uint8_t vx)
{
    printf("ld_delay_to_reg");
    registers[vx] = delay_timer;
}

// FX15
static void
ld_reg_to_delay(const uint8_t vx)
{
    printf("ld_reg_to_delay");
    delay_timer = registers[vx];
}

// FX18
static void
ld_sound(const uint8_t vx)
{
    printf("ld_sound");
    sound_timer = registers[vx];
}

// FX1E
static void
add_addr(const uint8_t vx)
{
    printf("add_addr");

    registers[0xf] = (address_register + registers[vx]) >
                     0x0FFF; // Set VF if overflow
    address_register += registers[vx];
}

// FX29
static void
ld_sprite(const uint8_t vx)
{
    printf("ld_sprite %u from V%.1x", registers[vx], vx);
#define SPRITE_SIZE (5) // Bytes
    ERRCHK(registers[vx] <= 0xf);
    address_register = 0x0 + registers[vx] * SPRITE_SIZE;
}

// FX33
static void
bcd(const uint8_t vx)
{
    const uint8_t val = registers[vx];
    printf("bcd %u", val);

    memory[address_register + 0] = registers[vx] / 100;
    memory[address_register + 1] = (registers[vx] / 10) % 10;
    memory[address_register + 2] = registers[vx] % 10;

    printf(": %u, %u, %u", memory[address_register],
           memory[address_register + 1], memory[address_register + 2]);
}

// FX55
static void
reg_dump(const uint8_t vx)
{
    printf("reg_dump");
    ERRCHK(vx < REGISTER_COUNT);
    for (uint8_t i = 0; i <= vx; ++i)
        memory[address_register + i] = registers[i];

    // address_register += vx + 1; // NOTE: ambiguous documentation whether or
    // not ar is incremented
}

// FX65
static void
reg_load(const uint8_t vx)
{
    printf("reg_load\n");
    ERRCHK(vx < REGISTER_COUNT);
    for (uint8_t i = 0; i <= vx; ++i) {
        ERRCHK(address_register + i < MEMORY_SIZE);
        registers[i] = memory[address_register + i];
        printf("V%.1x <= %u\n", i, memory[address_register + i]);
    }

    // address_register += vx + 1; // NOTE: ambiguous documentation whether or
    // not ar is incremented
}

/*
// Chip-8 instruction set
00E0 - CLS
00EE - RET
0nnn - SYS addr
1nnn - JP addr
2nnn - CALL addr
3xkk - SE Vx, byte
4xkk - SNE Vx, byte
5xy0 - SE Vx, Vy
6xkk - LD Vx, byte
7xkk - ADD Vx, byte
8xy0 - LD Vx, Vy
8xy1 - OR Vx, Vy
8xy2 - AND Vx, Vy
8xy3 - XOR Vx, Vy
8xy4 - ADD Vx, Vy
8xy5 - SUB Vx, Vy
8xy6 - SHR Vx {, Vy}
8xy7 - SUBN Vx, Vy
8xyE - SHL Vx {, Vy}
9xy0 - SNE Vx, Vy
Annn - LD I, addr
Bnnn - JP V0, addr
Cxkk - RND Vx, byte
Dxyn - DRW Vx, Vy, nibble
Ex9E - SKP Vx
ExA1 - SKNP Vx
Fx07 - LD Vx, DT
Fx0A - LD Vx, K
Fx15 - LD DT, Vx
Fx18 - LD ST, Vx
Fx1E - ADD I, Vx
Fx29 - LD F, Vx
Fx33 - LD B, Vx
Fx55 - LD [I], Vx
Fx65 - LD Vx, [I]
*/

void
instruction_cycle(void)
{
    static uint32_t start = 0;
    const uint32_t end    = SDL_GetTicks();
    if ((end - start) >= 1000 / TIMER_RATE) {
        if (delay_timer)
            --delay_timer;
        if (sound_timer) {
            printf("Beep!\a\n"); // Beep!
            --sound_timer;
        }
        start = end;
    }

    // Fetch
    const uint16_t opcode = convert_endianess(*((uint16_t*)(&memory[pc])));
    printf("%u (0x%.4x): 0x%.4x ", pc, pc, opcode);
    pc += OPCODE_SIZE;

    // Decode
    const uint16_t NNN = opcode & 0x0fff;
    const uint8_t NN   = opcode & 0x00ff;
    const uint8_t N    = opcode & 0x000f;
    const uint8_t X    = (opcode & 0x0f00) >> 8;
    const uint8_t Y    = (opcode & 0x00f0) >> 4;

    // Execute
    switch (opcode & 0xf000) {
    case 0x0000:
        if (opcode == 0x00EE)
            ret();
        else if (opcode == 0x00E0)
            cls();
        else
            chip8_syscall(NNN);
        break;
    case 0x1000:
        jmp(NNN);
        break;
    case 0x2000:
        call(NNN);
        break;
    case 0x3000:
        se(X, NN);
        break;
    case 0x4000:
        sne(X, NN);
        break;
    case 0x5000:
        se_reg(X, Y);
        break;
    case 0x6000:
        ld(X, NN);
        break;
    case 0x7000:
        add(X, NN);
        break;
    case 0x8000: {
        const uint16_t func_type = opcode & 0x000f;
        if (func_type == 0x0000) {
            ld_reg(X, Y);
        }
        else if (func_type == 0x0001) {
            or_reg(X, Y);
        }
        else if (func_type == 0x0002) {
            and_reg(X, Y);
        }
        else if (func_type == 0x0003) {
            xor_reg(X, Y);
        }
        else if (func_type == 0x0004) {
            add_reg(X, Y);
        }
        else if (func_type == 0x0005) {
            sub_reg(X, Y);
        }
        else if (func_type == 0x0006) {
            shr_reg(X);
        }
        else if (func_type == 0x0007) {
            subn_reg(X, Y);
        }
        else if (func_type == 0x000e) {
            shl_reg(X);
        }
        else {
            printf("Unknown operation at %lu: 0x%.4x ", pc - sizeof(opcode),
                   opcode);
            exit(EXIT_FAILURE);
        }
    } break;
    case 0x9000:
        sne_reg(X, Y);
        break;
    case 0xA000:
        ld_addr(NNN);
        break;
    case 0xB000:
        jmp_relative(NNN);
        break;
    case 0xC000:
        rnd(X, NN);
        break;
    case 0xD000:
        draw(X, Y, N);
        break;
    case 0xE000: {
        const uint16_t func_type = opcode & 0x00ff;
        if (func_type == 0x009e) {
            skip_p(X);
        }
        else if (func_type == 0x00a1) {
            skip_np(X);
        }
        else {
            printf("Unknown operation at %lu: 0x%.4x ", pc - sizeof(opcode),
                   opcode);
            exit(EXIT_FAILURE);
        }
    } break;
    case 0xF000: {
        const uint16_t func_type = opcode & 0x00ff;
        if (func_type == 0x000a) {
            getkey(X);
        }
        else if (func_type == 0x0007) {
            ld_delay_to_reg(X);
        }
        else if (func_type == 0x0015) {
            ld_reg_to_delay(X);
        }
        else if (func_type == 0x0018) {
            ld_sound(X);
        }
        else if (func_type == 0x001e) {
            add_addr(X);
        }
        else if (func_type == 0x0029) {
            ld_sprite(X);
        }
        else if (func_type == 0x0033) {
            bcd(X);
        }
        else if (func_type == 0x0055) {
            reg_dump(X);
        }
        else if (func_type == 0x0065) {
            reg_load(X);
        }
        else {
            printf("Unknown operation at %lu: 0x%.4x ", pc - sizeof(opcode),
                   opcode);
            exit(EXIT_FAILURE);
        }
    } break;
    default:
        printf("Unknown operation at %lu: 0x%.4x ", pc - sizeof(opcode),
               opcode);
        exit(EXIT_FAILURE);
    }
    printf("\n");
    // if ((opcode & 0xf000) & 0xD000) // Delay until the next refresh
    //    return; // Note: ambiguous documentation whether should delay or
    //    not
}

void
print_help(void)
{
    //clang-format off
    printf(
        R"(Usage: ./chip --rom <rom path> --ips <value> --displaymode <wrap|clamp>
Keys:
  esc
    1 2 3 4
    q w e r
    a s d f
    z x c v
)");
    //clang-format on
}

static bool
match(const char* a, const char* b)
{
    return !strcmp(a, b);
}

int
main(int argc, char** argv)
{
    char* path   = NULL;
    uint32_t ips = 200;
    displaymode  = CLAMP;

    for (int i = 1; i < argc; ++i) {
        if (match("--rom", argv[i])) {
            if (i + 1 >= argc)
                ERROR("Error processing --rom: Malformed argument");

            path = argv[i + 1];
            ++i;
        }
        else if (match("--ips", argv[i])) {
            if (i + 1 >= argc)
                ERROR("Error processing --ips: Malformed argument");

            ips = atoi(argv[i + 1]);
            ++i;
        }
        else if (match("--displaymode", argv[i])) {
            if (i + 1 >= argc)
                ERROR("Error processing --displaymode: Malformed argument");

            if (match("wrap", argv[i + 1]))
                displaymode = WRAP;
            else if (match("clamp", argv[i + 1]))
                displaymode = CLAMP;
            else
                ERROR("Error processing --displaymode argument: supply either "
                      "`wrap` or `clamp`");
            ++i;
        }
        else if (match("--help", argv[i])) {
            print_help();
            return EXIT_SUCCESS;
        }
        else {
            fprintf(stderr, "Invalid argument %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    if (!path) {
        ERROR("Did not supply valid rom path");
    }

    display_init();

    dump_rom(path);

    load_sprites();
    load_rom(path);

    uint32_t start = 0;
    while (running) {
        display_refresh(display);
        running = eval_input();

        instruction_cycle();

        const uint32_t end     = SDL_GetTicks();
        const uint32_t elapsed = end - start;

        const uint32_t time_per_cycle = 1000 / ips; // ms
        if (elapsed > time_per_cycle)
            start = end;
        else
            SDL_Delay(time_per_cycle - elapsed);
    }

    dump_memory();
    dump_memory_opcodes();
    dump_registers();
    display_quit();
    return EXIT_SUCCESS;
}
