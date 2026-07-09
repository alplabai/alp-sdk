# ALP-B007

The selected board preset does not list the selected SoM preset's `family:` in
`hosts_som_families:`.

## Fix

Use a board preset that hosts the SoM family, or define a compatible custom
board inline in `board.yaml`.
