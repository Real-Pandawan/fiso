# FISO — Flac ISO : présentation

FISO est un format ouvert et gratuit d'image de CD audio. Un fichier `.fiso` contient un disque complet :

* l'audio, compressé en **FLAC sans perte** (niveau 8 par défaut) — le PCM décodé est identique, bit pour bit, à la source ;
* la structure du disque : pistes, index, pré-gaps, piste cachée avant la piste 1, codes ISRC, MCN, drapeaux, table des matières ;
* les métadonnées, y compris l'appartenance à un album de plusieurs disques (`DISCNUMBER`, `DISCTOTAL`, `ALBUM_GROUP_ID`, identifiants MusicBrainz) ;
* les pochettes ;
* des empreintes SHA-256 pour contrôler l'intégrité.

Ce n'est ni une archive ZIP renommée, ni une image ISO 9660 : FISO a sa propre signature binaire (`89 46 49 53 4F 0D 0A 0A`), sa table de blocs, sa gestion de versions et ses règles de validation. La spécification est publique (CC0) : n'importe quel logiciel peut lire ou écrire des fichiers FISO sans dépendre de FISO Explorer.

## Choix techniques clés

**Un disque = une image.** Un coffret de 3 CD donne 3 fichiers, reliés par un identifiant commun. FISO Explorer les regroupe automatiquement et signale les disques manquants.

**Chronologie continue.** L'audio du disque est découpé en segments FLAC autonomes, coupés à chaque INDEX 01. Le pré-gap d'une piste est donc stocké à la fin de la piste précédente, exactement comme sur le disque. Conséquences : lecture sans aucune coupure, BIN/CUE reconstruit à l'identique, et chaque piste s'extrait en FLAC par simple copie, sans réencodage.

**Modifier les métadonnées ne touche jamais l'audio.** L'image est réécrite dans un fichier temporaire, les flux FLAC sont copiés octet pour octet (et revérifiés), puis le fichier remplace l'original de façon atomique : une coupure de courant ne peut pas corrompre l'image existante.

**Trois états d'intégrité distincts** : *fichier valide* (structure conforme), *audio intègre* (décodage complet, MD5 FLAC et SHA-256 du PCM conformes), *rip vérifié* (contrôle contre une référence externe — jamais déduit des deux autres).

## Télécharger et installer

Les installeurs de FISO Explorer sont publiés sur la page *Releases* du dépôt : macOS Apple Silicon et Intel (`.dmg`, macOS 12 ou plus récent), Windows 10/11 64 bits (`.exe`) et Linux 64 bits (`.AppImage`).

* **macOS** : ouvrir le `.dmg` et glisser *FISO Explorer* dans *Applications*. L'application n'étant pas notarisée par Apple, le premier lancement est bloqué : ouvrir *Réglages Système › Confidentialité et sécurité*, puis cliquer sur **Ouvrir quand même** en face du message concernant FISO Explorer. Cette étape n'est nécessaire qu'une fois.
* **Windows** : lancer l'installeur. Si SmartScreen affiche « Windows a protégé votre ordinateur », cliquer sur **Informations complémentaires**, puis **Exécuter quand même**.
* **Linux** : rendre l'AppImage exécutable (`chmod +x`) puis la lancer. Sous Ubuntu 22.04 et suivants, installer FUSE 2 (`sudo apt install libfuse2`, ou `libfuse2t64` sous Ubuntu 24.04).

Pour compiler soi-même le logiciel, voir [BUILDING.md](BUILDING.md).

## Ce que fait la version 1.1.0

* Créer une image depuis un **CD audio** (mode sécurisé ou rapide, pointeurs C2, correction du décalage de lecture, détection des pré-gaps, audio caché avant la piste 1, ISRC, MCN, CD-Text), depuis des fichiers FLAC/WAV ou depuis un fichier CUE. Les fichiers FLAC sont conservés tels quels, ou réencodés à la demande avec exactement les mêmes réglages pour toutes les pistes.
* Identifier le disque avec MusicBrainz (vignette de la pochette et nombre de pistes pour chaque édition) et récupérer les pochettes sur Cover Art Archive.
* Vérifier l'extraction avec **AccurateRip** ; l'état « Rip vérifié » n'est enregistré que si toutes les pistes correspondent.
* Ouvrir, explorer, écouter (lecture sans blanc entre les pistes), vérifier, modifier les métadonnées.
* Extraire en FLAC (sans réencodage), WAV, pochettes, métadonnées JSON.
* **Convertir une image en image CD audio gravable** (depuis l'accueil ou la fiche d'une image) : FISO Explorer crée un BIN/CUE au format CD audio, à graver avec n'importe quel logiciel de gravure (Burn, Toast, ImgBurn, cdrdao…). Les débuts de pistes sont placés sur les secteurs du CD (moins de 7 ms, sans blanc ajouté entre les pistes) et les images haute résolution ne sont converties qu'avec votre accord. L'image FISO, elle, reste une archive fidèle : la création ne modifie jamais l'audio.
* Afficher la pochette avant comme **icône des fichiers .fiso** dans le Finder (extension Quick Look ; bouton « Réactiver les icônes » dans les paramètres).
* Interface en anglais (par défaut), français, espagnol, allemand, portugais, italien, néerlandais et japonais ; thème clair et sombre.

Fonctions par système : l'extraction de CD fonctionne sous macOS, Windows et Linux (sous Linux, l'utilisateur doit appartenir au groupe `cdrom` ou `optical`). Seule l'icône des fichiers dans le Finder est propre à macOS ; sous Linux, une vignette est disponible en option.

## État du projet

La version 1.1.0 est la version actuelle (la 1.0.0 était la première version publique). Le format, la bibliothèque et l'application sont couverts par des tests automatiques sous macOS, Windows et Linux. L'extraction de CD et les icônes du Finder ont été testées avec un lecteur simulé et restent à confirmer sur du matériel réel : les retours sont les bienvenus dans la section *Issues* du dépôt.
