# FISO — Flac ISO — Format ouvert d'image de disque audio — Spécification v1.0

Statut : **projet avant gel de la v1.0** · Licence du document : **CC0 1.0** (domaine public) · Traduction française ; en cas de divergence, le texte anglais ([FISO_FORMAT_v1.0.md](FISO_FORMAT_v1.0.md)) fait foi.

Les mots DOIT, NE DOIT PAS, DEVRAIT, NE DEVRAIT PAS et PEUT s'entendent au sens de la RFC 2119 (MUST, MUST NOT, SHOULD, SHOULD NOT, MAY).

---

## 1. Objet et périmètre

Une image FISO (`.fiso`, type MIME `application/x-fiso-image`) contient **un disque audio physique** (ou une chronologie audio équivalente construite à partir de fichiers) dans un fichier unique :

* le programme audio complet, compressé sans perte en **FLAC** ;
* la structure du disque (pistes, index, pré-gaps, audio caché avant la piste 1, ISRC, MCN, drapeaux, TOC d'origine) ;
* les métadonnées descriptives, y compris l'appartenance à un album de plusieurs disques ;
* des images intégrées (pochette avant, pochette arrière…) ;
* des données d'intégrité (SHA-256 de chaque bloc, SHA-256 du PCM décodé par segment et pour toute la chronologie).

FISO n'est **pas** une archive ZIP ni une image ISO 9660, et ne conserve ni les sous-canaux physiques (P à W) ni la couche brute des trames Red Book. Il conserve le programme PCM audible ainsi que les informations de structure nécessaires pour reconstituer un CD-DA en Disc-at-Once (§9).

Les pistes de données des disques mixtes / CD-Extra **ne sont pas stockées** en v1.0. Leur existence est inscrite dans la table TOC (§5.6), afin qu'un lecteur sache que l'image n'est pas une copie complète du disque.

**Nom.** « FISO » signifie *Flac ISO* : un fichier qui contient un disque entier, comme une image ISO, avec l'audio en FLAC. Malgré son nom, une image FISO n'est pas un système de fichiers ISO 9660.

## 2. Conventions

* Tous les entiers sont **petit-boutistes** (little-endian), non signés sauf mention `i32` (complément à deux).
* Les offsets sont des positions absolues en octets depuis le début du fichier.
* `échantillon` désigne une **trame d'échantillons** (un échantillon par canal). Pour le CD-DA : 44 100 Hz, 2 canaux, 16 bits.
* Unités CD-DA : 1 secteur = 1/75 s = **588 échantillons** = **2 352 octets** de PCM.
* LBA : adresse logique de bloc au sens du Red Book, LBA 0 = MSF 00:02:00.
* Forme canonique du PCM (utilisée pour toutes les empreintes) : canaux entrelacés, entiers signés, petit-boutiste, `ceil(bits/8)` octets par échantillon. Pour le CD-DA, c'est exactement la disposition d'un fichier audio brut `.bin`.
* SHA-256 selon FIPS 180-4. CRC-32 = CRC-32/ISO-HDLC (polynôme 0x04C11DB7 réfléchi, init 0xFFFFFFFF, xorout 0xFFFFFFFF — le `crc32` de zlib).
* Les chaînes des structures binaires sont des champs ASCII de taille fixe complétés par des octets NUL.

## 3. Organisation du fichier

```
+---------------------------+  offset 0
| En-tête (96 octets)       |
+---------------------------+  offset 96 (aligné sur 8)
| Données des blocs ...     |  chaque bloc commence sur une frontière de 8 octets,
| (ordre libre)             |  les trous sont remplis d'octets nuls
+---------------------------+  table_offset (aligné sur 8)
| Table des blocs           |  chunk_count × 64 octets
+---------------------------+  file_size
```

La table des blocs est à la fin pour qu'un écrivain puisse produire l'audio en flux sans connaître les tailles à l'avance.

### 3.1 En-tête (96 octets)

| Offset | Taille | Champ | Valeur / règle |
|---:|---:|---|---|
| 0 | 8 | `magic` | `89 46 49 53 4F 0D 0A 0A` (`\x89FISO\r\n\n`) |
| 8 | 2 | `version_major` | `1` |
| 10 | 2 | `version_minor` | `0` |
| 12 | 4 | `header_size` | `96` |
| 16 | 4 | `file_flags` | `0` (réservé, DOIT valoir 0 en v1.0) |
| 20 | 4 | `chunk_count` | 1 … 65 536 |
| 24 | 8 | `table_offset` | multiple de 8, ≥ 96 |
| 32 | 8 | `file_size` | DOIT être égal à la taille réelle du fichier |
| 40 | 32 | `table_sha256` | SHA-256 de toute la table des blocs |
| 72 | 20 | réservé | zéro |
| 92 | 4 | `header_crc32` | CRC-32 des octets 0 … 91 |

La signature suit le modèle du PNG : le premier octet non ASCII détecte les transferts 7 bits, `\r\n` détecte une conversion CRLF→LF et le `\n` final isolé détecte une conversion LF→CRLF.

`table_offset + chunk_count × 64` DOIT être égal à `file_size`.

### 3.2 Gestion des versions

* Un lecteur DOIT refuser un fichier dont il n'implémente pas la `version_major`.
* Un lecteur qui implémente `1.x` DOIT accepter toute version `1.y`, en ignorant les blocs non critiques et les champs réservés qu'il ne comprend pas.
* Les versions mineures PEUVENT ajouter des types de blocs et des membres à la *fin* des objets JSON ; elles NE DOIVENT PAS modifier le sens ni la taille des champs binaires existants.

### 3.3 Entrée de la table des blocs (64 octets)

| Offset | Taille | Champ | Règle |
|---:|---:|---|---|
| 0 | 4 | `type` | quatre caractères ASCII, par ex. `FLAC` |
| 4 | 4 | `flags` | bit 0 `CRITICAL` ; autres bits à 0 |
| 8 | 8 | `offset` | multiple de 8, ≥ `header_size` |
| 16 | 8 | `length` | en octets |
| 24 | 32 | `sha256` | SHA-256 des données du bloc |
| 56 | 4 | `chunk_version` | version du contenu du bloc (1 pour tous les types v1.0) |
| 60 | 4 | réservé | zéro |

Règles :

* `offset + length ≤ table_offset` ; les blocs NE DOIVENT PAS se chevaucher.
* Un lecteur qui rencontre un `type` inconnu marqué `CRITICAL` DOIT refuser d'ouvrir le fichier. Les blocs inconnus non critiques DOIVENT être ignorés et DEVRAIENT être recopiés à l'identique lors d'une réécriture.
* L'ordre des entrées n'a d'importance que pour les blocs `FLAC`, référencés par leur index depuis `LAYT`.

### 3.4 Types de blocs définis en v1.0

| Type | Critique | Nombre | Contenu |
|---|---|---|---|
| `LAYT` | oui | exactement 1 | organisation du disque, binaire (§5) |
| `META` | oui | exactement 1 | métadonnées descriptives, JSON UTF-8 (§6) |
| `FLAC` | oui | ≥ 1 | un segment audio sous forme de flux FLAC autonome (§4) |
| `PICT` | non | 0 … n | image intégrée (§7) |
| `CDTX` | non | 0 … 1 | paquets CD-Text bruts lus sur le disque (paquets de 18 octets, CRC compris) (§7.1) |
| `RLOG` | non | 0 … 1 | journal d'extraction / de création, texte UTF-8, à titre informatif |

## 4. Audio : segments et chronologie

### 4.1 Chronologie

L'image représente une **chronologie** PCM continue de `total_samples` échantillons. L'échantillon 0 se trouve au LBA `timeline_start_lba` du disque d'origine (normalement 0, c'est-à-dire le début de la piste 1, INDEX 00 ou INDEX 01). Le lead-in et le pré-gap obligatoire de 150 secteurs avant le LBA 0 ne font pas partie de la chronologie.

### 4.2 Règle de découpage

La chronologie est découpée en **segments**, chacun stocké dans un bloc `FLAC`. En v1.0 les points de coupe sont fixes :

* l'**INDEX 01** de chaque piste ouvre un nouveau segment ;
* si l'INDEX 01 de la piste 1 se trouve après l'échantillon 0, l'audio qui le précède forme un **segment HTOA** (*Hidden Track One Audio*, type 0).

Un segment de piste s'étend donc de son INDEX 01 jusqu'à l'INDEX 01 de la piste suivante (ou la fin de la chronologie), et **le pré-gap (zone INDEX 00) de la piste N+1 est stocké à la fin du segment de la piste N**. Un pré-gap silencieux ne coûte presque rien après compression FLAC. Cette règle garde la chronologie sans coupure et rend chaque segment directement exportable en fichier FLAC, sans réencodage.

### 4.3 Contenu d'un bloc FLAC

* Un flux FLAC complet : marqueur `fLaC`, `STREAMINFO` en premier, éventuellement `SEEKTABLE`, `PADDING` et un `VORBIS_COMMENT` réduit à la chaîne de l'encodeur, puis les trames audio.
* Un écrivain NE DOIT PAS placer d'étiquettes dans un segment : pas de bloc `PICTURE`, et un éventuel `VORBIS_COMMENT` ne contient que la chaîne de l'encodeur et aucun commentaire (libFLAC en écrit toujours un). Les métadonnées sont dans `META`/`PICT` : les modifier ne touche jamais l'audio. Un lecteur DOIT ignorer les blocs `VORBIS_COMMENT`, `PICTURE` et `APPLICATION` des segments.
* La fréquence, le nombre de canaux et la résolution de `STREAMINFO` DOIVENT être ceux de `LAYT` ; `total samples` DOIT être égal à la longueur du segment et non nul ; le champ `MD5` DOIT être renseigné (non nul).
* Un écrivain DEVRAIT encoder avec l'équivalent de `flac -8` et le mode de vérification de libFLAC.

### 4.4 Conformité CD-DA

Le bit 0 de `LAYT.flags` (`CDDA`) NE DOIT être positionné que si : 44 100 Hz, 2 canaux, 16 bits, et chaque début/longueur de segment, début/longueur de piste et position d'index est un multiple de 588. Seules les images portant ce drapeau PEUVENT être gravées sur CD. Un écrivain NE DOIT PAS rééchantillonner ni requantifier en silence pour l'obtenir.

## 5. Bloc `LAYT` (binaire)

Organisation : en-tête (128 octets), puis `track_count` enregistrements de pistes, `index_count` enregistrements d'index, `segment_count` enregistrements de segments, `toc_entry_count` enregistrements TOC, dans cet ordre et sans remplissage. La longueur du bloc DOIT valoir `128 + 48·T + 16·I + 64·S + 16·C`.

### 5.1 En-tête (128 octets)

| Offset | Taille | Champ | Règle |
|---:|---:|---|---|
| 0 | 2 | `layt_version` | 1 |
| 2 | 2 | `flags` | bit 0 `CDDA`, bit 1 `HTOA`, bit 2 `PHYSICAL_SOURCE` (extrait d'un disque), bit 3 `HAS_TOC`, bit 4 `SYNTHESIZED_SILENCE` (une partie de l'audio est un silence numérique généré depuis un `PREGAP`/`POSTGAP` de CUE, non lu sur une source) |
| 4 | 4 | `sample_rate` | 1 … 655 350 |
| 8 | 1 | `channels` | 1 … 8 |
| 9 | 1 | `bits_per_sample` | 4 … 32 |
| 10 | 2 | réservé | 0 |
| 12 | 2 | `track_count` | 1 … 99 |
| 14 | 2 | `index_count` | ≥ `track_count` |
| 16 | 2 | `segment_count` | `track_count` ou `track_count + 1` (HTOA) |
| 18 | 2 | `toc_entry_count` | 0 … 101 |
| 20 | 1 | `first_track_number` | 1 … 99 |
| 21 | 1 | `last_track_number` | ≥ premier |
| 22 | 2 | réservé | 0 |
| 24 | 4 | `timeline_start_lba` (`i32`) | LBA de l'échantillon 0 |
| 28 | 4 | réservé | 0 |
| 32 | 8 | `total_samples` | > 0 |
| 40 | 32 | `timeline_pcm_sha256` | SHA-256 du PCM canonique de toute la chronologie |
| 72 | 16 | `mcn` | Media Catalog Number (13 chiffres ASCII) ou entièrement NUL |
| 88 | 40 | réservé | 0 |

### 5.2 Enregistrement de piste (48 octets)

| Offset | Taille | Champ | Règle |
|---:|---:|---|---|
| 0 | 1 | `number` | 1 … 99, strictement croissant de 1 en 1 |
| 1 | 1 | `flags` | bit 0 préaccentuation, bit 1 copie numérique autorisée, bit 2 quatre canaux |
| 2 | 2 | `first_index` | position du premier index de la piste dans la table des index |
| 4 | 2 | `index_count` | ≥ 1 |
| 6 | 2 | réservé | 0 |
| 8 | 8 | `start_sample` | position de l'INDEX 01 |
| 16 | 8 | `length_samples` | jusqu'à l'INDEX 01 de la piste suivante / fin de la chronologie |
| 24 | 16 | `isrc` | 12 caractères ASCII ou entièrement NUL |
| 40 | 8 | réservé | 0 |

### 5.3 Enregistrement d'index (16 octets)

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 1 | `track_number` |
| 1 | 1 | `index_number` (0 … 99) |
| 2 | 6 | réservé (0) |
| 8 | 8 | `sample_position` (chronologie) |

Règles : les enregistrements sont regroupés par piste dans l'ordre des pistes et triés par `index_number` ; les positions ne décroissent jamais sur l'ensemble de la table ; chaque piste a son INDEX 01 à `start_sample` ; l'INDEX 00 de la piste N (s'il existe) est dans `[début de la piste N-1, start_sample de N)` ; un INDEX ≥ 2 de la piste N est à l'intérieur de la piste N.

La **longueur du pré-gap** d'une piste vaut `start_sample − position(INDEX 00)`, ou 0 sans INDEX 00. Le pré-gap de la piste 1 est la zone HTOA lorsqu'elle existe.

### 5.4 Enregistrement de segment (64 octets)

| Offset | Taille | Champ | Règle |
|---:|---:|---|---|
| 0 | 4 | `chunk_index` | position dans la table des blocs d'un bloc `FLAC` |
| 4 | 1 | `kind` | 0 = HTOA, 1 = piste |
| 5 | 1 | `track_number` | 0 pour le HTOA |
| 6 | 2 | réservé | 0 |
| 8 | 8 | `start_sample` | |
| 16 | 8 | `length_samples` | > 0 |
| 24 | 32 | `pcm_sha256` | SHA-256 du PCM canonique du segment |
| 56 | 8 | réservé | 0 |

Les segments sont dans l'ordre de la chronologie, contigus, et couvrent exactement `[0, total_samples)`. Le début et la longueur d'un segment de piste DOIVENT être ceux de son enregistrement de piste. Chaque bloc `FLAC` est référencé exactement une fois.

### 5.5 Empreinte de la chronologie

`timeline_pcm_sha256` est calculée sur le PCM canonique de tous les segments mis bout à bout dans l'ordre (= le fichier BIN du §9.2).

### 5.6 Enregistrement TOC (16 octets) — informatif

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 1 | `track_number` (1 … 99, `0xAA` = lead-out) |
| 1 | 1 | `control` (quartet de contrôle du canal Q) |
| 2 | 1 | `adr` |
| 3 | 1 | `session` (à partir de 1) |
| 4 | 4 | `start_lba` (`i32`) |
| 8 | 1 | `stored` (1 si l'audio de la piste est dans l'image, 0 pour une piste de données) |
| 9 | 7 | réservé |

Quand `HAS_TOC` est positionné, la table reproduit la TOC lue sur le disque physique, pistes de données et lead-out compris, et permet de recalculer exactement le Disc ID MusicBrainz. Pour une image créée à partir de fichiers, un écrivain PEUT synthétiser une TOC (`HAS_TOC` positionné, `PHYSICAL_SOURCE` non positionné).

## 6. Bloc `META` (JSON)

JSON UTF-8, sans BOM, ≤ 16 Mio. Schéma formel : `spec/schemas/fiso-meta-1.schema.json`. Le membre de premier niveau `"schema"` DOIT valoir `"fiso.meta/1"`.

```json
{
  "schema": "fiso.meta/1",
  "album": {
    "title": "Exemple", "artist": "Artiste", "date": "1999-04-12",
    "genre": "Rock", "label": "Label", "catalog_number": "CAT-01",
    "country": "FR", "barcode": "0123456789012",
    "group_id": "8b1c…-uuid",
    "musicbrainz": { "release_id": "…", "release_group_id": "…",
                     "album_artist_id": "…", "disc_id": "…" }
  },
  "disc": { "number": 2, "total": 3, "subtitle": "Live" },
  "tracks": [ { "number": 1, "title": "Intro", "artist": "Artiste",
                "musicbrainz": { "recording_id": "…", "track_id": "…" } } ],
  "cdtext": { "album_title": "…", "album_performer": "…" },
  "tags": { "COMMENT": "paires clé/valeur libres (noms Vorbis comment)" },
  "provenance": {
    "source": "cd | files | cue", "creator": "FISO Explorer 1.0.0",
    "created": "2026-11-08T00:00:00Z",
    "rip": { "mode": "secure | fast", "drive": "…", "read_offset": 6, "c2": true,
             "retried_sectors": 0, "unrecovered_errors": 0,
             "verified": false, "verification": "" },
    "encoding": { "encoder": "libFLAC 1.4.3", "compression": 8, "copied_tracks": [2, 3] },
    "derived_from": "nom de l'image d'origine, si cette image a été dérivée d'une autre image FISO",
    "cd_mastering": { "alignment": "shift", "moved_boundaries": 2, "max_shift_ms": 3.9,
                      "end_padding_samples": 547, "converted_from": "96.0 kHz / 24 bit / stereo" }
  }
}
```

Membres obligatoires : `schema`, `album.title`, `disc.number`, `disc.total` (1 ≤ number ≤ total ≤ 999), `tracks` (un objet par piste de `LAYT`, associé par `number`). Tout le reste est facultatif ; un membre absent signifie *inconnu*, jamais une valeur vide. Un lecteur NE DOIT PAS échouer sur un membre inconnu.

Une image FISO est une archive : un écrivain DOIT conserver l'audio source sans modification lorsqu'il crée une image depuis des fichiers, un fichier CUE ou un disque. Un écrivain PEUT proposer, comme opération séparée et explicite, de dériver une *version CD* d'une image existante (un nouveau fichier ; l'original n'est jamais modifié) afin de pouvoir la graver en CD-DA (drapeau `CDDA`). Cette image porte `provenance.derived_from` et `provenance.cd_mastering`, qui indique ce qui a été modifié : débuts de pistes déplacés sur la limite de secteur la plus proche (`shift` ; aucun son ajouté ni retiré entre les pistes), silence ajouté après la dernière piste (`end_padding_samples`) et, uniquement avec l'accord de l'utilisateur, une conversion de format (`converted_from`). Sa présence indique que l'image est une version CD et non l'archive.

`provenance.encoding` indique comment les trames audio ont été produites : l'encodeur et son niveau de compression, et dans `copied_tracks` les pistes dont les trames FLAC ont été copiées telles quelles depuis les fichiers sources (encodées ailleurs, avec des réglages inconnus). En l'absence de `copied_tracks`, toutes les pistes ont été encodées par l'encodeur indiqué avec les mêmes réglages. FLAC étant sans perte, ces choix changent les données compressées, jamais l'audio décodé.

Correspondance avec les étiquettes usuelles : `ALBUM`=album.title, `ALBUMARTIST`=album.artist, `DISCNUMBER`=disc.number, `DISCTOTAL`=disc.total, `DISCSUBTITLE`=disc.subtitle, `ALBUM_GROUP_ID`=album.group_id, `MUSICBRAINZ_ALBUMID`=album.musicbrainz.release_id.

### 6.1 Albums de plusieurs disques

Une image = un disque physique. Les images d'un même album sont regroupées, par ordre de priorité, sur :

1. un `album.musicbrainz.release_id` identique ;
2. un `album.group_id` identique (UUID partagé par les images d'un album lorsqu'aucune édition MusicBrainz n'existe) ;
3. à défaut, un lecteur PEUT proposer un regroupement approximatif sur (`album.title`, `album.artist`, `disc.total`), mais DOIT le présenter comme une supposition.

## 7. Bloc `PICT` (binaire)

| Champ | Taille |
|---|---|
| `picture_type` (code ID3v2 APIC / FLAC : 3 = pochette avant, 4 = pochette arrière, 0 = autre…) | 4 |
| `mime_length` | 4 |
| `mime` (ASCII ; les écrivains v1.0 utilisent `image/jpeg` ou `image/png`) | `mime_length` |
| `description_length` | 4 |
| `description` (UTF-8) | `description_length` |
| `width`, `height`, `color_depth`, `colors_used` | 4 × 4 |
| `data_length` | 4 |
| `data` | `data_length` |

La longueur du bloc DOIT être égale à la somme des champs. Maximum 64 Mio. Les octets de l'image ne sont pas fiables : un lecteur DOIT les décoder avec un décodeur d'images isolé ou durci, et ne jamais les exécuter.

### 7.1 Bloc `CDTX`

Les paquets CD-Text tels que renvoyés par le lecteur (READ TOC format 0101b), sans l'en-tête de réponse de 4 octets : une suite de paquets de 18 octets (type de paquet, numéro de piste, numéro de séquence, bloc/position de caractère, 12 octets de texte, CRC-16). La longueur DOIT être un multiple de 18, au plus 1 Mio. Un lecteur PEUT le décoder ; les métadonnées de référence restent toujours celles de `META`. Un logiciel de gravure DEVRAIT reproduire ces paquets tels quels lorsque le numéro de la première piste est 1, et sinon construire le CD-Text à partir de `META`.

## 8. Niveaux d'intégrité

Un vérificateur conforme rapporte trois états **indépendants** :

| État | Signification | Contrôles |
|---|---|---|
| **Fichier valide** | la structure est conforme | signature, version, CRC de l'en-tête, tailles/offsets, SHA-256 de la table, SHA-256 de chaque bloc, règles LAYT/META/PICT |
| **Audio intègre** | l'audio décodé est identique à ce qui a été stocké | chaque flux FLAC se décode sans erreur, le MD5 de STREAMINFO correspond, les SHA-256 PCM des segments et de la chronologie correspondent |
| **Rip vérifié** | l'audio source a été contrôlé par rapport à une référence externe | uniquement si `provenance.rip.verified` vaut vrai **et** que `provenance.rip.verification` nomme la méthode ; jamais déduit |

## 9. Reconstitution d'un CD-DA

### 9.1 Disc-at-Once

Écrire la chronologie telle quelle en mode DAO. La piste N commence à son INDEX 01 ; l'INDEX 00 et les autres index sont écrits comme marques d'index du sous-canal. Aucun silence n'est inséré entre les pistes. ISRC, MCN, préaccentuation et drapeaux de copie viennent de `LAYT` ; le CD-Text vient de `CDTX` s'il existe, sinon de `META` (`cdtext`, puis titres de l'album et des pistes).

Correspondance recommandée pour un logiciel qui grave directement une image FISO (moteurs de gravure qui décrivent une piste comme *pré-gap + données*). L'implémentation de référence ne grave pas ; elle exporte un BIN/CUE CD-DA (format d’extraction `bin-cue`) :

* piste 1 : pré-gap = 150 secteurs de silence numérique suivis de la zone HTOA `[0, début de la piste 1)` ; données = `[début de la piste 1, frontière suivante)` ;
* piste N > 1 : pré-gap = `[INDEX 00, INDEX 01)` (vide sans INDEX 00, c'est-à-dire sans blanc) ; données = `[INDEX 01, frontière suivante)` ;
* la *frontière suivante* est l'INDEX 00 de la piste suivante s'il existe, sinon son INDEX 01, sinon la fin de la chronologie ;
* les INDEX 02 … 99 sont écrits comme points d'index relatifs à l'INDEX 01.

Sur le disque obtenu, le LBA *x* contient le secteur *x* de la chronologie. Un logiciel de gravure DEVRAIT proposer de relire le disque et de le comparer à la chronologie, en recherchant le décalage combiné lecture/écriture.

### 9.2 Export BIN/CUE

Le fichier BIN est le PCM canonique de toute la chronologie. La feuille CUE utilise un seul `FILE … BINARY` et, pour chaque piste, `TRACK nn AUDIO`, éventuellement `FLAGS`/`ISRC`, `INDEX 00` (s'il existe), `INDEX 01` et les index suivants, positions converties en `mm:ss:ff` depuis le début du BIN. Lorsque la chronologie ne commence pas au LBA 0 ou que l'image n'est pas `CDDA`, un exportateur DOIT avertir qu'un disque exact ne peut pas être reconstitué.

## 10. Exigences pour les écrivains

* Écrire dans un fichier temporaire du dossier de destination, `fsync`, puis renommer de façon atomique ; ne jamais modifier une image existante sur place.
* Une modification des métadonnées DOIT recopier les blocs `FLAC` octet pour octet.
* Écrire l'en-tête en dernier.

## 11. Exigences et limites pour les lecteurs

Avant toute allocation, un lecteur DOIT vérifier : `header_size == 96`, `chunk_count ≤ 65 536`, tous les offsets/longueurs dans le fichier sans débordement d'entier, longueur de `LAYT` cohérente avec ses compteurs, `META` ≤ 16 Mio, `PICT` ≤ 64 Mio, règles de pistes/segments/index du §5. Toute violation rend le fichier invalide.

## 12. Type de média et extension

Extension `.fiso`. Type MIME suggéré `application/x-fiso-image`. Uniform Type Identifier (macOS) `fr.retrospare.fiso-image`, conforme à `public.data`. Signature freedesktop.org : chaîne `\x89FISO\r\n\n` à l'offset 0.

Les gestionnaires de fichiers et générateurs de vignettes DEVRAIENT afficher la pochette avant (`PICT` de type 3, sinon le premier `PICT`) comme icône d'un fichier `.fiso`. L'implémentation de référence fournit une extension de vignettes Quick Look (macOS) et une commande `fiso thumbnail IMAGE SORTIE` pour les générateurs de vignettes freedesktop.org.

## 13. Historique

* 1.0 (projet) — version initiale ; contenu de `CDTX` (§7.1), correspondance pour la gravure (§9.1), recommandation d'icône (§12).
