# Usage

`Usage: ./chip --rom <rom path> --ips <value> --displaymode <wrap|clamp>`

A good range for ips (instructions per second) is usually 100-500 (200 by default).

Display mode can be `wrap` or `clamp`. Some games work better with wrap and some with clamp.

# Dependencies

SDL2.

# Keys

```
esc
    1 2 3 4
    q w e r
    a s d f
    z x c v
```

# FAQ

Why does everything blink?

> Chip-8 has instructions only for clearing the screen and XORring pixels. To update a sprite, one needs to execute two instructions: the first one unsets the pixels and the second one redraws. If the framebuffer is drawn occasionally in the middle of the update, then sprites appear to blink. Drawing is also used for collision detection so f.ex. the paddles in Pong blink even when stationary.

Some games do not work

> Some instructions are undocumented or ambiguously defined; one instruction that works one way on one interpreter may work differently on another. Programmers used whatever worked on their machine so here we are. Changing ips or displaymode may fix some games.
