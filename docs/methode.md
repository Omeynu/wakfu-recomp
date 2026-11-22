# Méthode — de la copie du jeu au C++ qui compile

Chaque étape se relance avec les scripts du dépôt ; les fichiers du jeu restent dans l'espace
de travail (`work/` → lien ignoré par Git).

## 1. Extraction STFS (`scripts/stfs_extract.py`)

Aucun extracteur STFS Linux maintenu n'était disponible (Velocity et wxPirs ne tournent que sous
Windows ; `xenia-vfs-dump` impose de compiler Xenia). Extracteur écrit d'après Free60 :

- en-tête STFS : taille d'en-tête en `0x340`, descripteur de volume en `0x379` ;
- les deux paquets sont « female » (`block_separation & 1 = 1`) : une table de hachage par niveau,
  seul cas géré (refus explicite sinon) ;
- adressage des blocs (formules Free60/Velocity) ; **chaque bloc lu est vérifié contre son SHA-1**
  de la table de niveau 0 ; noms de fichiers validés (pas de `..`, pas de `/`).

Test : `scripts/test_stfs.py <paquet TU>` (formules d'adressage, extraction réelle, `default.xexp`
au magic `XEX2`, détection d'un bloc corrompu).

## 2. XEX patché et image (`tools-src/xexdump`)

Petit outil lié à `XenonUtils` : applique le `.xexp` de la TU #2 avec `XexPatcher` (le même code
que XenonRecomp), écrit le XEX patché, vide l'image chargée, liste sections et imports.
Le nom de section `.XBMOVIE` occupe 8 caractères sans zéro final : affichage tronqué à 8.

## 3. Analyse (`scripts/analyse_xex.py`) → `docs/analyse.md`

En-têtes optionnels, bibliothèques XDK, imports par ordinal (noms : tables Xenia fournies avec
XenonRecomp), nombre d'appels `bl` par thunk, mots-clés dans les chaînes.

## 4. Fonctions d'aide du compilateur (`scripts/find_helpers.py`)

Recherche du premier mot de chaque routine, puis vérification **instruction par instruction** :

| Routine | Premier mot | Vérification |
|---|---|---|
| `__savegprlr_14` | `std r14,-0x98(r1)` = `F9C1FF68` | `std r14..r31` à `-0x98+8k`, `stw r12,-8(r1)`, `blr` |
| `__restgprlr_14` | `ld r14,-0x98(r1)` = `E9C1FF68` | `ld r14..r31`, `lwz r12,-8(r1)`, `mtlr r12`, `blr` |
| `__savefpr_14` / `__restfpr_14` | `stfd`/`lfd f14,-0x90(r12)` | f14..f31 à `-0x90+8k`, `blr` |
| `__savevmx_14` / `__restvmx_14` | `li r11,-0x120` + `stvx`/`lvx v14,r11,r12` | 18 paires, `blr` |
| `__savevmx_64` / `__restvmx_64` | juste après la routine 14..31 (+0x94) | 64 paires `li r11,-0x400+16k` + opcode 4 (VMX128), `blr` |

`setjmp` (0x823DF6C0 : crochet optionnel `bnectr`, puis sauvegarde LR/CR/f14-f31/r13-r31/VMX
dans le tampon pointé par r3) et `longjmp` (0x823DFC70 : `val==0 → 1`, rechargement depuis r7)
sont utilisés par Lua 5.1 (8 et 19 appels) : renseignés.

## 5. Tables de sauts (`scripts/find_switches.py` + `config/switches_manual.toml`)

XenonAnalyse ne trouvait **qu'une table** : ses motifs fixes sont cassés par les `nop` que ce
compilateur (C 16.0.11886) intercale (`lis/addi/nop/add/mtctr/bctr`). Remplacé par :

1. pour chaque `bctr`, remonter jusqu'au `bgt crN` (ou `bgtlr`) puis au `cmplwi crN,rI,N` ;
2. **émuler** la ligne droite `bgt → bctr` pour chaque index 0..N (lectures dans l'image) :
   tolère `nop`, `slwi`, tables `lwzx` absolues ou `lbzx`/`lhzx` relatives ;
3. refus si instruction non gérée, registre inconnu, ou `rI` modifié avant le `bctr` ;
4. cibles hors de la fonction (`.pdata`) = refus.

Résultat : 140 tables automatiques + 7 relues à la main (garde éloignée, garde sur une copie
du registre, ou absente) = **147 tables**. Contrôle de couverture : sur 386 `bctr`, 147 tables,
238 appels indirects (vtables, pointeurs de fonctions), 1 refus justifié (appel virtuel en
0x82250818, pas un `switch`). La table vue par XenonAnalyse fait partie du résultat, mêmes cibles.

`base` = adresse du `bctr` (XenonRecomp applique la table au prochain `bctr` rencontré).

16 fonctions feuilles **sans entrée `.pdata`** contiennent une table : XenonRecomp les coupait
au `bctr` (509 erreurs « jump outside function »). Bornes calculées par le même script (début =
cible de `bl` la plus proche ; fin = prochain début de fonction après la plus grande cible),
relues (début après `blr`/`b`/bourrage, fin sur `blr` ou `b` interne) et mises dans `functions`.
Une table est intégrée au code (0x8225FD54, 22 mots).

## 6. Données dans `.text` (`invalid_instructions`)

Trous non couverts par `.pdata` : bourrage nul et couples [gestionnaire, données] placés avant
les fonctions à gestion d'exceptions : `__C_specific_handler` (thunk 0x82626DDC, 73 fois),
`__CxxFrameHandler` (0x823E1970, 6 fois), autre gestionnaire (0x823F15E0, 1 fois).

## 7. Patch XenonRecomp (`patches/xenonrecomp-wakfu.patch`)

Le commit épinglé ne traduisait pas 52 instructions utilisées par le jeu (7 868 occurrences dans
695 fonctions sur 15 437) ; elles devenaient des commentaires (code faux). Ajouts :

- branches `bdzf`, `bdnzt` ; `bdnzf` généralisé (l'original supposait le bit `eq`) ; `cror`, `crorc` ;
- `addc`, `addme`, `subfze` (+ formes `.`), `eqv` ;
- chargements/stores avec mise à jour : `lhzu lhau sthu lfsu lfdu stfsu stfdu lbzux lhzux lwzux
  ldux lfsux lfdux stbux sthux stdux` ;
- VMX : `lvehx vspltish vslh vsrh vsrah vrlh vsrab vsubshs vaddsbs vsububm vaddsws vmaxsh vminsh
  vavguh vcmpgtsh(.) vcmpequh(.) vcmpgtsw(.) vpkswss vpkshss`, bit RC de `vcmpgtuh.` ;
- VMX128 : `vpkswss128 vpkswus128 vpkshss128 vpkuhus128 vsel128 vcfpuxws128`.

Conventions reprises de l'existant : vecteurs inversés en bloc (les `pack` prennent vB pour la
moitié basse) ; retenue `XER[CA]` calculée sur 32 bits (comme `adde`/`subfe` upstream).

**Test différentiel** (`tests/insn/run_tests.py`) : chaque corps est assemblé une fois ; QEMU
(`qemu-ppc -cpu 7400`, `qemu-ppc64` pour `ldux`/`stdux`) sert de référence, le même corps est
recompilé par XenonRecomp et exécuté par un pilote C++ ; états comparés (GPR, CR, XER[CA], CTR,
FPR, VR, 64 octets de mémoire). VMX128 : `.long` côté recomp (décodage vérifié dans le C++
généré), équivalent VMX côté QEMU. **63 cas, 405 exécutions, 0 échec** ; une mutation volontaire
(`vsrah` masqué à 0x7) est bien détectée (6 échecs).

Écart connu, hérité de l'upstream : `stfs*` convertit avec `float()` (arrondi) alors que le
matériel tronque ; identique dès que la valeur est représentable en simple précision.

## 8. Vérification de compilation (`check/`)

`check/CMakeLists.txt` : tout `out/ppc/*.cpp` en bibliothèque statique, Clang ≥ 18, `-O2`,
mêmes options qu'UnleashedRecompLib (`-fno-strict-aliasing -ffp-model=strict -march=sandybridge`).
`check/link_check.py` : lien `--whole-archive` sans stubs → les symboles manquants doivent être
exactement les imports du XEX ; puis lien avec stubs vides → doit réussir.

## 9. Shaders

Les `.xpu`/`.xvu` de `Media/Shaders` sont des conteneurs D3D Xenos (drapeaux `0x102A1100` pixel,
`0x102A1101` vertex) : XenosRecomp en mode fichier (HLSL) puis dossier (DXC → SPIR-V).

## 10. Optimisations XenonRecomp (`config/wakfu.toml`)

**Désactivées depuis le 18/11** : avec elles, le décodeur vidéo du jeu produit des blocs (écran-titre cassé depuis
`fe08cb5`, vu sous RADV et lavapipe, textures décodées sur CPU comprises) ; sans elles, l'écran-titre est correct et la
cadence en jeu est la même (28,4-29,3 contre 28,9-29,7 img/s, vblank limitant). L'option fautive n'est pas identifiée :
la chercher une à une (j4 contrôle maintenant les blocs vert/magenta) avant d'en réactiver une. Analyse d'origine :

Les huit options d'UnleashedRecomp étaient activées. Chacune suppose quelque chose du code invité ;
vérifié sur le C++ généré (analyse « lu avant écrit » sur le graphe de flot de chaque fonction,
asserts `sub_8227D0E8`, `sub_8227D0B0`, `sub_824A9E88` sans retour) et dans `runtime/src` :

| Option | Hypothèse | Constat sur Wakfu |
|---|---|---|
| `skip_lr` | LR ne sert qu'aux retours, pas d'exceptions | 1 seul `blrl` (0x823F13D0, appel de funclet d'exception) : le patch prend la cible dans le `mtlr rX` précédent ; les `mflr` restants sauvent LR. Le runtime ne lit LR que pour ses diagnostics (`wk::DiagLr`, 0 avec l'option). |
| `skip_msr` | MSR sans effet observable | les 189 `mfmsr` ne servent qu'au `mtmsrd` suivant ; le runtime ne lit pas `ctx.msr`. |
| `ctr_as_local`, `xer_as_local`, `reserved_as_local`, `cr_as_local` | volatils, jamais vivants à l'entrée d'une fonction | aucune lecture à l'entrée hors `setjmp` invité (redirigé vers l'hôte) et un fragment de `memset` jamais appelé ; `lwarx`/`stwcx.` toujours dans la même fonction. |
| `non_argument_as_local` (r0 r2 r11 r12 f0 v32-v63) | jamais passés en argument | r12 lu à l'entrée par les funclets d'exception (pointeur de cadre) et par la sonde de pile 0x823E0ED4 (elle ne sonde plus rien : sans effet). |
| `non_volatile_as_local` (r14-r31 f14-f31 v14-v31 v64-v127) | conservés par l'appelé, jamais passés en argument ; `setjmp` | aucune lecture réelle à l'entrée (les cas restants : chemins impossibles). Aux 8 `setjmp` (Lua, libjpeg), MSVC recharge depuis la pile ce qui sert après le retour ; seul le pointeur de cadre r31 (fixé avant, jamais modifié) est conservé. |

Seul le code d'exceptions C++/SEH en pâtit : il ne marchait déjà pas (`RtlUnwind`, `RtlRaiseException` stubs).

**Correction (2026-11-17) : les funclets `__finally`.** Le constat ci-dessus était faux sur un point : les blocs
`__finally` de MSVC (CRT, tas `RtlpAllocateHeap`, fichiers) sont aussi appelés sur le chemin normal, par `bl`
depuis leur parent, et lisent ses registres (r12 = cadre, r28-r31 = pointeur vers la section critique).
En variables locales à 0, ils appelaient `RtlLeaveCriticalSection(0)` ou `(0x20)` (diagnostic `[diag] section
critique invalide`), la section n'était jamais libérée et le jeu se bloquait (thread XUI et thread audio dans
`RtlEnterCriticalSection`, sauvegarde du joueur). Correctif dans le patch XenonRecomp : option
`registers_in_context`, liste de fonctions générées avec tous leurs registres dans `ctx` (et leurs appels
`__save*`/`__rest*` conservés, ces aides étant elles aussi générées sur `ctx`) ; les autres fonctions
gardent les locales. La liste (funclets + appelants
directs, ≈ 170) est produite par `scripts/find_context_functions.py` (analyse de flux par blocs du C++
généré avec une liste vide) ; quelques faux positifs (chemins impossibles) sont sans danger. Quand la liste
n'est pas vide, `ppc_config.h` ne retire plus ces registres de `PPCContext`.
Le runtime ne lit ni n'écrit les registres concernés (crochets `PPC_FUNC` : r3-r10 et r13 seulement).
