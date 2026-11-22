# Jouer sur un PC Windows et signaler un problème

## Installer et lancer

1. Télécharger `WakfuRecomp.exe` depuis la page [Releases](https://github.com/Omeynu/wakfu-recomp/releases) et l'ouvrir (double-clic).
2. Accueil : choisir « Installation normale » (recommandé : jeu dans `%LOCALAPPDATA%\WakfuRecomp`, données dans
   `%APPDATA%\WakfuRecomp`) ou « Portable » (tout dans `<dossier de l'exe>\WakfuRecomp`, données dans `...\userdata` ;
   refusé, avec la raison, sous Program Files, dans OneDrive, en lecture seule ou avec moins de 3 Go libres).
3. Deux champs « Jeu (Islands of Wakfu) » et « Mise à jour (TU 2) » : glisser-déposer (un ou deux fichiers n'importe
   où sur la fenêtre : ils sont classés d'après leur contenu) ou « Parcourir ». Paquet STFS, image ISO/XISO ou dossier
   extrait. Chaque champ affiche aussitôt ✔ « Jeu de base » / ✔ « Mise à jour 2 », ou ✖ et la cause (pas un paquet
   Xbox 360, autre jeu, jeu et TU inversés, autre version de la TU, fichier illisible ou tronqué). « Recompiler »
   est grisé tant que les deux champs ne sont pas ✔ ; une mauvaise version (SHA-1 de l'image, contrôlé à la
   préparation) ramène aux champs avec la cause. Ensuite : recompilation (barre de progression ; de quelques minutes
   à une heure : noter la durée), réglages, puis « Jouer ».
4. Lancements suivants : `WakfuRecomp.exe` ouvre directement l'écran « Jouer » (boutons Réglages et Recompiler).
5. Quitter le jeu à la manette : BACK + START maintenus 3 secondes.

Rien de recompilé n'est distribué : tout est produit sur le PC à partir de votre copie du jeu.

## Réglages

`%APPDATA%\WakfuRecomp\config.toml` (créé au premier lancement ; détail dans `options.md`). Pour un écran 1440p :

```toml
[video]
mode = "sans_bordure"
echelle_rendu = 2
afficher_images_par_seconde = true

[post]
agrandissement = "fsr1"   # image agrandie par AMD FSR 1 (plus nette en 1440p)
```

Ces réglages sont aussi dans la fenêtre de `WakfuRecomp.exe` (bouton Réglages).

## Signaler un problème

Mettre dans `config.toml` :

```toml
[confort]
console = false
```

Le journal complet est alors écrit dans `%APPDATA%\WakfuRecomp\journal.txt` ; les deux lancements précédents
restent dans `journal.1.txt` et `journal.2.txt`. La dernière ligne (`[sortie] code N : raison`) dit pourquoi le jeu
s'est arrêté. Pour signaler un problème, ouvrir une issue sur GitHub avec :

- `journal.txt` (et `config.toml`) ;
- une ou deux captures d'écran du problème ;
- en une phrase : ce qui a été fait (menu, zone, action) et ce qui s'est passé.

Pour un journal du jeu plus détaillé (messages internes du jeu), lancer `jouer.cmd` depuis une invite de commandes
après `set WAKFU_GAMELOG=1`.

## Ce qui est utile à signaler

- Durée de la première compilation, et si elle échoue : la dernière page de la console.
- La fenêtre s'ouvre-t-elle ? Plein écran / sans bordure, 1440p.
- Images par seconde affichées dans le titre (menus et en jeu).
- Manette (boutons, sticks, vibrations), clavier.
- Son (musique, effets, coupures).
- Sauvegarde : « Continue Game » après avoir quitté.
