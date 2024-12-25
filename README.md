# TPMExtractor
TPMExtractor is a tool for extracting assets from the game "Star Wars: Episode I - The Phantom Menace"

Object and Level assets are stored seperately in TPM (The Phantom Menace).

Objects (models/textures/animations) are stored within .BAF files
which are further contained within "big.lab" which is present in compressed form as "BIG.Z"
on the install disc.

Levels are contained within compressed .B3D files which are located on the install disc under /GAMEDATA/LEVEL/.

TPMExtractor is capable of processing these incoming data directly from the install disc.

## Usage
`tpmextractor InstallDiscPath DumpDirPath`

Example:
`tpmextractor H: "c:/tpm dump"`


## Credits
- KnightCop - Project lead
- Strike [@bahstrike](https://github.com/bahstrike) - Code, File analysis
- shiny [@shinyquagsire23](https://github.com/shinyquagsire23) - .B3D decompression
- Wolfgang Frisch [@wfr](https://github.com/wfr) - .Z decompression
