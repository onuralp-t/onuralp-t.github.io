# From "Hello WASM" to "How are you, WASM!?"
Check out [my article](https://onuralp-t.github.io/#article-2-how-to-wasm) for a WASM introductory text. In this article, we'll be moving on from "Hello WASM" to "How are you, WASM!?"

Or to state it more clearly: we'll look into how WASM works on a fundamental level.
# The WASM module
Before walking through how to include the WASM module into the web project, let's see what's inside the program that is going to be compiled into a WASM module in the first place.
```c
// main.c
#include <stdint.h>

// JS-defined:
void wasm_log(const char *msg);

constexpr uint32_t SCREEN_WIDTH = 800u;
constexpr uint32_t SCREEN_HEIGHT = 600u;

struct {
  uint8_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT * 4u];
} screen;

uint32_t screen_get_width(void)  { return SCREEN_WIDTH; }
uint32_t screen_get_height(void) { return SCREEN_HEIGHT; }
uint8_t* screen_get_pixels(void) { return screen.pixels; }
uint32_t screen_get_pixels_size(void) { return SCREEN_WIDTH * SCREEN_HEIGHT * 4u; }

void update(uint32_t frame_number) {
    for (uint32_t y = 0u; y < SCREEN_HEIGHT; y++)
    {
        for (uint32_t x = 0u; x < SCREEN_WIDTH; x++)
        {
            uint32_t idx = (y * SCREEN_WIDTH + x) * 4u;
            screen.pixels[idx + 0] = x + frame_number;
            screen.pixels[idx + 1] = y + frame_number;
            screen.pixels[idx + 2] = 150u;
            screen.pixels[idx + 3] = 255u;
        }
    }
    wasm_log("message from WASM update()!");
}
```
Built with:
```bat
clang --target=wasm32 ^
  -O2 ^
  -std=c23 ^
  -ffreestanding -nostdlib ^
  -Wl,--no-entry ^
  -Wl,--export-all ^
  -Wl,--allow-undefined ^
  -o main.wasm main.c
```
## Using JS functions from inside our WASM module
At the very top, we see:
```c
void wasm_log(const char *msg);
```
This `wasm_log` function is only declared in `main.c`, not defined. Later, when we load the WASM module into our JS program, we'll define `wasm_long` inside JS. The `-Wl,--allow-undefined` CLI flag in our build command facilitates this.
## Accessing WASM variables from our JS script
```c
constexpr uint32_t SCREEN_WIDTH = 800u;
constexpr uint32_t SCREEN_HEIGHT = 600u;

struct {
  uint8_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT * 4u];
} screen;
```
First two constants are defined using C23's `constexpr` qualifier to have type-checked constants in global scope, as alternative to macros or enums.

When we do `console.log(wasm.exports)` later in our JS script, we will see `screen` but not our `SCREEN_WIDTH` and `SCREEN_HEIGHT` listed there.

The `-Wl,--export-all` flag in our build command facilitates the export of all functions and objects, but this doesn't apply to simple variables.

To solve this problem of not being able to access our variables from JS, we have few possible strategies in front of us. The first sensible option seems to be to pack our semantically related variables into a single struct (What I would do if I wasn't writing a WASM program).
### The Correct-looking Solution
If we have a struct like this (defined again in global scope):
```c
#define WIDTH 800u
#define HEIGHT 600u
struct {
  uint32_t SCREEN_WIDTH;
  uint32_t SCREEN_HEIGHT;
  uint8_t pixels[WIDTH * HEIGHT * 4u];
} screen = {.SCREEN_WIDTH=WIDTH, .SCREEN_HEIGHT=HEIGHT};
```
1. Later when we load the WASM module into our JS script, we will see `screen` listed in the exports list.
2. There, we will get a pointer to the base of the `screen` struct.
3. Then, we will have to walk through each element of the struct while being mindful of the memory layout of our struct (memory padding!) and how big every element is, manually calculating the offsets to each element and casting/viewing them as the correct type.
This is perfectly possible, yet error-prone. Why don't we just let Clang calculate the memory addresses for us?
### The Correcter Solution
```c
constexpr uint32_t SCREEN_WIDTH = 800u;
constexpr uint32_t SCREEN_HEIGHT = 600u;

struct {
  uint8_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT * 4u];
} screen;

uint32_t screen_get_width(void)  { return SCREEN_WIDTH; }
uint32_t screen_get_height(void) { return SCREEN_HEIGHT; }
uint8_t* screen_get_pixels(void) { return screen.pixels; }
uint32_t screen_get_pixels_size(void) { return SCREEN_WIDTH * SCREEN_HEIGHT * 4u; }
```
Although getter functions get a bad rep, I think this is a good use case for them. Defining a getter function inside our WASM module offloads all the burden of memory calculations to the compiler, and gives us a human-readable and not-error-prone way of accessing our variables. These will most likely be used in the initialization part anyways, and they're not that bad even if they're used elsewhere.

We can also use fancy attributes to steer our compiler to optimize them in a nicer way like:
```c
__attribute__((pure))
uint32_t screen_get_width(void)  { return SCREEN_WIDTH; }
```

The `update` function is relatively simple, it just does some animation, you don't have to comprehend it in a deep way.
## Inside JavaScript
```js
const utf8_decoder = new TextDecoder("utf-8");
function wasm_log(c_str) {
	const str_start = c_str;
	let str_end = str_start;
	while (u8_view[str_end] !== 0) { // C's null terminated strings
		str_end++;
	}
    const message = utf8_decoder.decode(u8_view.slice(str_start, str_end));
    console.log(message);
}
  
  const wasm_compiled = await WebAssembly.instantiateStreaming(
    fetch("./main.wasm"),
    {
      env: { wasm_log }, // define what WASM module asks for
    },
);
    const wasm = wasm_compiled.instance;
    const we = wasm.exports;
    console.log("wasm.exports: ", we);
```
In the `env` element, we put in the implementation of the `wasm_log` function we call from inside our WASM module, and we create some helper variables like `we` which indexes all our WASM exports. To access our WASM exports, we use the strategy we talked about earlier:
```js
function read_screen_from_wasm() {
	const screen_width = we.screen_get_width();
	const screen_height = we.screen_get_height();
	const screen_pixels = new Uint8ClampedArray(we.memory.buffer, we.screen_get_pixels(), screen_width * screen_height * 4);
	return { screen_width, screen_height, screen_pixels };
}
const { screen_width, screen_height, screen_pixels } = read_screen_from_wasm(we.screen);
```

We then resize our HTML canvas to the width/height we accessed from the WASM module:
```js
	const canvas = document.getElementById("canvas");
	canvas.style.width = `${screen_width}px`;
	canvas.style.height = `${screen_height}px`;
	canvas.getContext("2d").scale(screen_width, screen_height);
```

After that, to write pixels to our canvas, we need to create a view into our `screen_pixels` array in the form of JavaScript's `ImageData` (we can only blit ImageData to canvas, we can't put screen_pixels to canvas directly):
```js
const imageData = new ImageData(screen_pixels, screen_width, screen_height);
```

And for the grand finale, we define what is essentially a rendering loop. We create the equivalent of a `while(true)` loop that updates the WASM memory with WASM functions, and then uses a view onto the WASM memory to blit it to our HTML canvas, and then repeat:
```js
	let frame = 0;
	function loop() {
    we.update(frame++); // update the screen_pixels data which lives in we.memory.buffer
    ctx.putImageData(imageData, 0, 0); // imageData is a view on screen_pixels which is in we.memory.buffer
    requestAnimationFrame(loop);
	}
	loop();
```
