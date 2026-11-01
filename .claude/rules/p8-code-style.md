---
paths:
  - "**/*.cpp"
  - "**/*.c"
  - "**/*.h"
  - "**/*.hpp"
---

# p8 Code Style

Full reference: `doc/code_style.md`. Defaults are non-negotiable unless the user explicitly overrides them.

## Golden Rules

1. All identifiers (files, types, functions, variables) are **lowercase** with `_` as the only word separator. No camelCase, no PascalCase.
2. **Every** type and **every** variable carries a prefix. No prefix = wrong.
3. Preprocessor `#define` names are the **only** ALL_CAPS identifiers.
4. C functions are prefixed with the module name (e.g. `p8_`). C++ class methods start with a verb (`do_`, `get_`, `set_`, `is_`, `create_`, ...).
5. Format with the repo's `.clang-format` before finishing.
6. Every function **definition** in `.c` / `.cpp` files is preceded by a separator line of 120 `/` characters.
7. Comments are short and plain: one line by default, two at most. See [Comments](#comments).

## File Naming

- `my_file_for_something.cpp`, `my_file_for_something.hpp`, `my_file_for_something.h`
- Never `MyFile.cpp`, never `myFile.cpp`, never `my-file.cpp`.

## Type Prefixes (used when declaring a type)

Single-letter prefix + `_` + lowercase name.

| Prefix | Meaning              | Example                   |
|--------|----------------------|---------------------------|
| `i`    | signed integer       | `typedef int32_t i_count;`|
| `u`    | unsigned integer     | `uint32_t mu_val;`        |
| `x`    | utf char             | `x_char ix_letter;`       |
| `z`    | size_t               | `size_t lz_len;`          |
| `b`    | bool                 | `bool ib_flag;`           |
| `c`    | class                | `class c_my_class`        |
| `d`    | double               | `double md_value;`        |
| `f`    | float                | `float lf_ratio;`         |
| `p`    | pointer              | `void *mp_data;`          |
| `r`    | reference            | `c_obj &or_out;`          |
| `v`    | r-value reference    | `c_obj &&iv_tmp;`         |
| `o`    | object (instance)    | `c_my_class lo_inst;`     |
| `s`    | structure            | `struct s_my_struct`      |
| `e`    | enum type / values   | `enum class e_state { e_idle, e_run };` |
| `t`    | template             | `template<typename t_x>`  |
| `l`    | function ptr / lambda| `typedef int (*l_cb)(int);`|
| `h`    | handle / custom type | `typedef int32_t h_p8_id;`|

**This table is exhaustive.** No other single-letter type prefixes exist. In particular there is no `a` for "array" — use `o` (object) for any STL container (`std::vector`, `std::map`, `kit::c_lst`, etc.).

## Variable Prefixes (access/visibility, then type)

`<access><type>_name`

Access part:

- Function parameters: `i` (in), `io` (in/out), `o` (out, input ignored)
- Scope: `g` (global / file-static), `l` (local)
- Members: `m` (class/struct member)

Examples:
- `ip_ctx`     — input pointer
- `iou_count`  — in/out unsigned
- `op_result`  — output pointer
- `gs_config`  — global struct instance
- `lz_size`    — local size_t
- `mp_data`    — member pointer
- `ml_timer_value` — member function-pointer/lambda
- `mu_val1`    — member unsigned

### `o` Disambiguation

- `o` as the **first** letter = output parameter (access).
- `o` as the **second** letter = object (type).
- Compound prefix order is therefore `<access><type>`, e.g. `oc_data` = output class, `lo_inst` = local object, `moc_child` is **wrong** (members can't be outputs).

## Functions

- C function: `<module>_<verb>_<noun>` → `p8_register_current_thread`, `p8_mtk_emit`.
- C++ method: starts with a **verb** → `acquire_buffer`, `release_buffer`, `get_size`, `set_name`, `is_ready`.
  - **Wrong:** `buffer_acquire` ❌, `size_get` ❌ — noun before verb violates the rule.
- Static file-scope helpers in `.cpp` files: same as C++ methods — start with a verb, no module prefix needed (they are not public API).
- Constructors/destructors follow class name (`c_my_class::c_my_class(...)`).

## Function Separators

Every function **definition** in `.c` / `.cpp` files must be preceded by a separator line of exactly 120 `/` characters:

```cpp
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static bool parse_size(const char *ip_str, size_t &oz_result)
{
    ...
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool cp8_core::init_buffer_pool(const char *ip_max, const char *ip_init)
{
    ...
}
```

Inside `extern "C"` blocks, the separator follows the same indentation as the function body.

This rule applies to **function definitions** only (`.c`, `.cpp` files), not to declarations in headers.

## Preprocessor Defines

ALL_CAPS is **exclusively** for `#define` macros. `static constexpr`, `const`, and `enum` values use normal lowercase prefixes:

```cpp
#define P8_MAX_CHANNELS 64                        // OK — #define
static constexpr uint32_t lu_max_channels = 64;   // OK — constexpr
static constexpr uint32_t LU_MAX_CHANNELS = 64;   // WRONG — not a #define
```

## Comments

A person reads every comment and has no time for long or tangled text.

- One short line by default, two at most. Plain words, no essays.
- Say why the code is there, not how it works step by step or what the old code did.
- Longer reasoning goes to `doc/`, not into the code.

```cpp
// after the drain, so descriptors used by this batch are sent before its data   // OK

// Service buffers are collected only once the data batch is complete: a          // WRONG — an essay
// writer queues a descriptor before it writes the first record that ...
```

## Canonical Example

```cpp
#include "my_header_file.h"

#define MY_DEFINE 0

enum class e_my_enum
{
    e_val_1,
    e_val_2,
    e_vals_count
};

struct s_my_struct
{
    uint32_t mu_val1;
    int32_t  mi_val2;
};

static const s_my_struct gs_my_global_val = {0, 0};

class c_my_class
{
    void *mp_data = nullptr;
public:
    explicit c_my_class(uint32_t iu_init_var);
    bool do_process(const s_my_struct *ip_in, s_my_struct *op_out);
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void my_function(c_my_class *&or_object)
{
    or_object = new c_my_class(10);
}
```

## Pre-Submit Checklist

- [ ] All new files use `lower_snake_case.{c,cpp,h,hpp}`.
- [ ] Every new type has a single-letter type prefix (`c_`, `s_`, `e_`, `h_`, `l_`, `t_`).
- [ ] Every new variable has `<access><type>_` prefix; no bare names.
- [ ] Function parameters use `i`/`io`/`o` access correctly.
- [ ] No camelCase or PascalCase anywhere except inside untouched third-party code.
- [ ] `#define`s are ALL_CAPS; no other identifier is.
- [ ] C functions begin with the module prefix; C++ methods begin with a verb.
- [ ] C++ methods begin with a verb (`acquire_x`, not `x_acquire`).
- [ ] No invented type prefixes — only those listed in the Type Prefixes table.
- [ ] `static constexpr` / `const` use lowercase with prefixes, not ALL_CAPS.
- [ ] Every function definition in `.c` / `.cpp` is preceded by a 120-char `///...` separator.
- [ ] Comments are short and plain: one line by default, two at most.
- [ ] Code is formatted with the project `.clang-format`.

## When in Doubt

Open `doc/code_style.md` and follow it literally. If a rule appears to conflict with surrounding existing code, match the existing file's style locally and flag the inconsistency to the user instead of silently mixing styles.
