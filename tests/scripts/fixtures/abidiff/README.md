# abidiff golden fixtures

Real output of libabigail `abidiff` 2.9.0 (Ubuntu `abigail-tools 2.9-3`,
`libabigail8 2.9-3`), gcc `-g -O0`, recorded for `test_check_abidiff.py`
(#2851).  The `*.txt` files are stdout, the `*.rc` files the exit status.

| fixture | pair | exit |
|---|---|---|
| `break.txt` | `src/v1` vs `src/v2`: removed function `gone`, `sig` signature change, `struct cfg` size change (seen through `use_cfg`), removed variable `g_level`, changed variable `g_mode` | 12 |
| `type_plus_param.txt` | `src/typeplus/v1` vs `v2`: `take(struct cfg *)` where the struct grows AND the function gains a parameter | 12 |
| `soname_only.txt` | `src/v1` built twice, SONAME `libalp_sdk.so.0` vs `libalp_sdk.so.0.17` | 12 |

Regenerate (abidiff exactly as `scripts/check_abidiff.py` calls it):

```bash
gcc -g -O0 -shared -fPIC -Isrc/v1/include src/v1/lib.c -Wl,-soname,libalp_sdk.so.0 -o v1.so
gcc -g -O0 -shared -fPIC -Isrc/v2/include src/v2/lib.c -Wl,-soname,libalp_sdk.so.0 -o v2.so
gcc -g -O0 -shared -fPIC -Isrc/v1/include src/v1/lib.c -Wl,-soname,libalp_sdk.so.0.17 -o s2.so
abidiff --no-added-syms --headers-dir1 src/v1/include --headers-dir2 src/v2/include v1.so v2.so > break.txt
abidiff --no-added-syms --headers-dir1 src/v1/include --headers-dir2 src/v1/include v1.so s2.so > soname_only.txt
```

(Compile each `.so` from a directory where the source file is named `lib.c`;
the report embeds `lib.c:N:M` locations.)  Do not hand-edit the outputs.

`type_plus_param.txt` uses the same gcc flags with `-Isrc/typeplus/{v1,v2}/include`
and `src/typeplus/{v1,v2}/lib.c`.
