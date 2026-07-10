# `metadata/e1m/`

Vendored, **verbatim** snapshot of the E1M standard's machine-readable
pinout, owned upstream by
[`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec).

| File | Upstream | Contents |
|------|----------|----------|
| `pinout-v1.json`   | `pinout/v1.json`   | E1M (35×35, `FC_35x35`) — 312 pads |
| `pinout-x-v1.json` | `pinout/x-v1.json` | E1M-X (45×65, `FC_45x65`) — 496 pads |
| `e1m-spec.lock`    | —                  | Pinned source commit + per-file `{id, version, pad_count}` |

Each pad carries `{id, silkscreen, default, instance?, alt[]}` — the pad
coordinate, its printed net name, its default signal kind, and the
peripheral instance it defaults to. This is the fixed geometry + default
function assignment of the E1M standard; the per-SoM realization (which
silicon pin backs each pad) lives in `metadata/e1m_modules/<family>/*.tsv`.

## Do not edit by hand

These files are regenerated, never hand-edited:

```sh
python3 scripts/sync_e1m_spec.py            # bump to e1m-spec@main
python3 scripts/sync_e1m_spec.py --ref v1.0 # pin a tag/commit
python3 scripts/check_e1m_pinout.py         # validate + cross-check (offline)
```

The upstream `loom-v1` schema is vendored alongside at
`metadata/schemas/loom-v1.schema.json`. Rationale and the
public/private-boundary reasoning are in
[ADR 0019](../../docs/adr/0019-vendor-e1m-spec-pinout-snapshot.md) and
[`docs/e1m-pinout.md`](../../docs/e1m-pinout.md).
