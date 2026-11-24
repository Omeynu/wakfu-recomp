# Chapitre 3 — Feuille de route

[Sommaire](README.md) · Précédent : [Problèmes et solutions](problemes/README.md)

Dans l'ordre prévu. Une option nouvelle reste désactivée par défaut tant qu'elle n'est pas validée, et doit tenir
60 images/s.

1. *Fait (v0.1.3 et v0.1.4)* : cadence sous Windows (minuteur), CPU des portables (fil de rendu -32 % par image en
   grande zone), rendu en **1440p** (définition interne x1 à x4 ; 1440p à 60 images/s au 99e centile sur la RX 6700).
   Aussi en v0.1.3 : les trois morceaux courts du point 7 (installateur sans fenêtre, `journal.log`, niveau 2 de
   RecompLauncher), mode portable supprimé.
2. *Fait (v0.1.4)* : **options rapides** : filtrage anisotrope, son coupé hors de la fenêtre, zone morte des sticks,
   intensité des vibrations, touche de capture d'écran, choix de l'écran et position de la fenêtre mémorisée, copie
   de secours des sauvegardes.
3. *Fait (v0.1.4)* : **image** : niveaux de MSAA (2x, 4x, 8x), ombres en plus haute définition, préréglages
   (Économie / Équilibré / Qualité).
4. *Fait (v0.1.4)* : **vérifications dans le jeu** : la coopération locale ne marchait pas (joueur 2 jamais connecté),
   réparée avec l'attribution des joueurs ; une seule langue de voix ; pas de flou de mouvement ni de profondeur de
   champ à désactiver ; le jeu ne produit que de la stéréo (détail dans `docs/options.md`, « Vérifié dans le jeu »).
5. **Options longues** : remappage de la manette, affichage des performances en jeu, notifications de succès,
   anticrénelage de la transparence, icônes de manette Xbox/PlayStation, filtre pour daltoniens.
6. **Fin de projet** : son surround 5.1/7.1, **plus de 60 images/s** (essai seulement : mesurer, abandonner si la
   logique du jeu dépend de la cadence). Pas de mods : aucun n'existe pour ce jeu (recherche du 23/11/2026).
7. **RecompLauncher**, le reste (Windows d'abord ; Linux suit sans bloquer) (doc développeur et schéma de recomplauncher.com, relus le 23/11/2026) :
   - *Fait (v0.1.3)* : niveau 2 dans `packaging/recomplauncher/recomplauncher.json`. Il contient le jeu et
     la mise à jour 2 en deux `inputs` (empreinte SHA-256 de la mise à jour), 13 réglages de `config.toml`, les
     dossiers des sauvegardes et du journal, et des messages pour les codes de sortie. Le schéma le valide.
   - *Installateur sans fenêtre* : avec `RECOMPLAUNCHER=1`, pas de console et pas d'attente d'Entrée quand
     l'installation échoue (aujourd'hui, elle bloquerait le lanceur). Écrire `progress <0..1> <texte>` aux étapes
     [1/5] à [5/5].
   - *Mode portable supprimé* (avec le correctif de l'installateur) : code de l'installateur et de la fenêtre
     (accueil, `portable.txt`), doc et tests retirés, ainsi que les entrées portables de `recomplauncher.json`. Au
     premier lancement, une seule fois, `<exe>\WakfuRecomp\userdata` (Linux : `<exe>/wakfu-recomp/userdata`) est
     copié vers le dossier normal des données, sans rien supprimer ni écraser.
   - *Journal* : `journal.txt` renommé en `journal.log` (et `journal.1.log`, `journal.2.log`), le lanceur ne joignant
     que des `.log`. Un ancien `journal.txt` est laissé tel quel. README, `docs/options.md` et modèle d'issue à jour.
     L'installateur sans console écrit `journal-installation.log` dans le dossier des données ; le message du code 1
     y renvoie.
   - *Progression* : tranches fixes, outils 0 à 0,05, préparation 0,05 à 0,15, XenonRecomp 0,15 à 0,3, compilation
     0,3 à 1 (au rythme des lignes `[n/m]` de ninja).
   - *Niveau 3* : étape `prepare` (`--sans-lancer {input:game} {input:update}`, `timeout` d'une heure au moins).
     `version` : compteur augmenté à la main quand le code généré change (XenonRecomp, correctifs) ; règle notée
     dans `CONTRIBUTING.md`. `done` : témoin `{local}/wakfu-recomp/pret`, écrit à la fin de la compilation et
     supprimé quand elle recommence (Windows ignore la casse : `%LOCALAPPDATA%\WakfuRecomp\pret`). Limite :
     un `XDG_DATA_HOME` personnalisé sous Linux recompile une fois de plus. `keeps_copy` :
     oui, `folders.game` = `{local}/wakfu-recomp/work/game`, et `start` sans arguments. Version minimale du lanceur
     indiquée dans le README.
   - *Big Picture* : avec `RECOMPLAUNCHER_MODE=bigpicture`, sans bordure pour ce lancement seulement (sauf si le
     joueur a choisi `plein_ecran`), sans réécrire `config.toml`. Avec `RECOMPLAUNCHER_PAD`,
     choisir les icônes Xbox ou PlayStation (lié aux icônes de manette du point 5).
   - *Linux* : avant la première compilation, le lanceur écrit les réglages dans `~/.config/WakfuRecomp`. Il trouve
     le bon chemin une fois `wakfu` compilé.
   - *À vérifier avant publication* (liste à suivre par le développeur sous Windows, publication une fois tout
     vert) : la reconnaissance du paquet XBLA (`ids` 584109AA, laissé
     de côté pour l'instant), `{input:update}` sans `into`, et les clés TOML `section/clé`.
   - *SDL3* : passage de SDL2 à SDL3 (conseillé par la doc), juste avant le point 5, après la vérification de la
     coopération à deux manettes (point 4).
