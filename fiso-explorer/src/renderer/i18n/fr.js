// FISO Explorer — French. SPDX-License-Identifier: Apache-2.0
window.I18N.fr = {
  'nav.home': 'Accueil',
  'nav.create': 'Créer',
  'nav.open': 'Ouvrir…',
  'nav.settings': 'Paramètres',
  'nav.help': 'Aide',
  'nav.recent': 'Images ouvertes',
  'nav.album': 'Album',

  'home.title': 'Vos disques, en un seul fichier.',
  'home.lead':
    'Une image FISO conserve un CD audio complet — audio FLAC sans perte, structure du disque, pochettes et métadonnées — dans un fichier unique et ouvert.',
  'home.create': 'Créer une image FISO',
  'home.create.d': 'À partir de fichiers FLAC/WAV ou d’une image BIN/CUE.',
  'home.open': 'Ouvrir une image FISO',
  'home.open.d': 'Explorer, écouter et vérifier un disque.',
  'home.extract': 'Extraire une image FISO',
  'home.extract.d': 'Pistes FLAC ou WAV, pochettes, métadonnées.',
  'home.drop': 'Glissez un fichier .fiso dans la fenêtre pour l’ouvrir.',

  'ex.disc': 'Disque {n} / {t}',
  'ex.tracks': '{n} pistes',
  'ex.track1': '1 piste',
  'ex.play': 'Lire',
  'ex.extract': 'Extraire',
  'ex.verify': 'Vérifier',
  'ex.meta': 'Métadonnées',
  'ex.codec': 'Audio',
  'ex.size': 'Taille FISO',
  'ex.pcm': 'PCM d’origine',
  'ex.ratio': 'Taux',
  'ex.mbid': 'Disc ID MusicBrainz',
  'ex.integrity': 'Intégrité',
  'ex.valid': 'Fichier valide',
  'ex.audio': 'Audio intègre',
  'ex.rip': 'Rip vérifié',

  'st.yes': 'oui',
  'st.no': 'non',
  'st.unchecked': 'non vérifié',
  'st.noref': 'aucune référence',

  'col.title': 'Titre',
  'col.length': 'Durée',
  'col.pregap': 'Pré-gap',
  'col.size': 'Taille',
  'col.isrc': 'ISRC',

  details: 'Détails de la piste',
  'details.none': 'Sélectionnez une piste pour voir ses détails.',
  'details.indexes': 'Index',
  'details.start': 'Début (échantillon)',
  'details.length': 'Longueur (échantillons)',
  'details.flags': 'Drapeaux',
  'details.pre': 'Préaccentuation',
  'details.dcp': 'Copie autorisée',
  'details.4ch': '4 canaux',

  htoa: 'Piste cachée avant la piste 1 ({d})',

  untitled: 'Sans titre',

  track: 'Piste {n}',

  'album.discs': 'Disques de cet album',
  'album.missing': 'manquant',
  'album.guess': 'regroupement supposé (aucun identifiant commun)',

  'cr.title': 'Créer une image FISO',
  'cr.source': 'Source',
  'cr.meta': 'Informations',
  'cr.output': 'Fichier de sortie',
  'cr.cd': 'CD audio',
  'cr.cd.d': 'Lecture sécurisée depuis un lecteur CD',
  'cr.files': 'Fichiers audio',
  'cr.files.d': 'FLAC ou WAV, une piste par fichier',
  'cr.cue': 'Image BIN/CUE',
  'cr.cue.d': 'Fichier .cue avec BIN, WAV ou FLAC',
  'cr.add': 'Ajouter des fichiers…',
  'cr.choosecue': 'Choisir un fichier .cue…',
  'cr.nosource': 'Aucune source choisie.',
  'cr.passthrough': 'Les fichiers FLAC compatibles sont conservés tels quels, sans réencodage.',
  'cr.compression': 'Compression FLAC',
  'cr.compression.d': 'Niveau 8 recommandé : plus lent, plus compact, toujours sans perte.',
  'cr.choose': 'Choisir l’emplacement…',
  'cr.go': 'Créer l’image',
  'cr.nooutput': 'Aucun fichier choisi.',

  'meta.album': 'Album',
  'meta.artist': 'Artiste de l’album',
  'meta.date': 'Date',
  'meta.genre': 'Genre',
  'meta.label': 'Label',
  'meta.disctotal': 'Nombre de disques dans l’album',
  'meta.discnum': 'Disque actuellement extrait',
  'meta.subtitle': 'Sous-titre du disque',
  'meta.join': 'Associer à un disque existant…',
  'meta.joined': 'Associé à l’album « {a} » (identifiant commun).',
  'meta.front': 'Pochette avant',
  'meta.back': 'Pochette arrière',
  'meta.pick': 'Choisir…',
  'meta.remove': 'Retirer',
  'meta.tracks': 'Pistes',
  'meta.trackartist': 'Artiste (si différent)',
  'meta.err.disc': 'Le numéro du disque doit être compris entre 1 et le nombre total de disques.',
  'meta.save': 'Enregistrer',
  'meta.edit.title': 'Modifier les métadonnées',
  'meta.edit.note': 'Seules les métadonnées sont réécrites : les flux FLAC sont copiés à l’identique, sans réencodage.',

  'mb.title': 'MusicBrainz',
  'mb.identify': 'Identifier ce disque',
  'mb.search': 'Rechercher',
  'mb.artist': 'Artiste',
  'mb.album': 'Album',
  'mb.none': 'Aucune édition trouvée. Essayez une recherche par artiste et titre.',
  'mb.choose': 'Plusieurs éditions correspondent : choisissez la bonne.',
  'mb.disc': 'Disque de l’édition',
  'mb.apply': 'Appliquer',
  'mb.loading': 'Interrogation de MusicBrainz…',
  'mb.covers': 'Télécharger les pochettes (Cover Art Archive)',
  'mb.mismatch': 'Attention : ce disque de l’édition a {a} pistes, l’image en a {b}.',
  'mb.applied': 'Informations appliquées. Vérifiez-les avant d’enregistrer.',

  'xt.title': 'Extraire',
  'xt.format': 'Format',
  'xt.flac': 'Pistes FLAC',
  'xt.flac.d': 'Copie directe, sans réencodage',
  'xt.wav': 'Pistes WAV',
  'xt.wav.d': 'PCM décodé, identique à l’original',
  'xt.cover': 'Pochettes',
  'xt.meta': 'Métadonnées (JSON)',
  'xt.scope': 'Pistes',
  'xt.all': 'Toutes les pistes',
  'xt.sel': 'Sélection ({n})',
  'xt.template': 'Modèle de nom',
  'xt.tokens': 'Jetons : {track:02} {title} {artist} {album} {albumartist} {disc} {disctotal} {year}',
  'xt.htoa': 'Inclure la piste cachée (avant la piste 1)',
  'xt.overwrite': 'Remplacer les fichiers existants',
  'xt.dest': 'Dossier de destination',
  'xt.go': 'Extraire',
  'xt.done': '{n} fichier(s) écrit(s)',

  'reveal.folder': 'Afficher dans le dossier',
  reveal: 'Afficher dans le Finder',

  'job.encode': 'Encodage FLAC',
  'job.verify': 'Vérification',
  'job.extract': 'Extraction',
  'job.copy': 'Réécriture',
  'job.cancel': 'Annuler',
  'job.cancelled': 'Opération annulée. Aucun fichier partiel n’a été conservé.',
  'job.wait': 'Préparation…',

  'rep.title': 'Image créée',
  'rep.pcm': 'Taille PCM originale',
  'rep.fiso': 'Taille de l’image FISO',
  'rep.ratio': 'Taux de compression réel',
  'rep.pass': '{n} piste(s) FLAC conservée(s) sans réencodage',
  'rep.open': 'Ouvrir l’image',
  'rep.warnings': 'Avertissements',

  'ver.title': 'Rapport de vérification',
  'ver.valid.d': 'Signature, version, en-tête, table des blocs et empreintes SHA-256 de chaque bloc.',
  'ver.audio.d': 'Décodage complet de chaque flux FLAC, contrôle MD5 FLAC et SHA-256 du PCM par segment et pour tout le disque.',
  'ver.rip.d': 'Contrôle par rapport à une référence externe (par ex. AccurateRip). Jamais déduit des deux autres états.',
  'ver.segments': 'Segments',
  'ver.verified': 'Vérifié : {m}',

  'set.title': 'Paramètres',
  'set.lang': 'Langue',
  'set.theme': 'Apparence',
  'set.system': 'Système',
  'set.light': 'Clair',
  'set.dark': 'Sombre',
  'set.outdir': 'Dossier de sortie par défaut',
  'set.template': 'Modèle de nom des pistes extraites',
  'set.cache': 'Cache MusicBrainz',
  'set.clear': 'Vider le cache',
  'set.cleared': 'Cache vidé.',
  'set.engine': 'Moteur',
  'set.default': 'Par défaut',
  'set.choose': 'Choisir…',

  'help.title': 'Aide',
  'help.spec': 'Lire la spécification FISO v1.0',
  'help.site': 'Site du projet',
  'help.limits': 'Limites connues de cette version',

  'about.title': 'À propos',

  ok: 'OK',

  cancel: 'Annuler',

  close: 'Fermer',

  error: 'Erreur',

  next: 'Continuer',

  'err.nobin': 'Le moteur « fiso » est introuvable ({p}). L’application a peut-être été mal assemblée.',
  'err.notfiso': 'Seuls les fichiers .fiso peuvent être ouverts ici.',

  'cv.title': 'Convertir en image CD audio gravable',

  'ex.tocd': 'Convertir pour CD',

  'cv.home.d':
    'Crée une image BIN/CUE au format CD audio, à graver avec votre logiciel de gravure habituel. L’image FISO reste intacte.',
  'cv.d':
    'Une image FISO conserve l’audio tel qu’il a été archivé. Pour graver un CD, FISO Explorer crée une image BIN/CUE séparée, au format CD audio, que tout logiciel de gravure sait graver :',
  'cv.asis': 'cette image est déjà au format CD audio : l’audio est copié tel quel, échantillon pour échantillon ;',
  'cv.align':
    'les débuts de pistes sont placés sur les secteurs du CD (1/75 s) : déplacement de moins de 7 ms, aucun son ajouté ni retiré entre les pistes ; un peu de silence complète la fin du disque ;',
  'cv.conv': 'l’audio, en {f}, est converti en 44,1 kHz / 16 bits / stéréo (rééchantillonnage de haute qualité et dither) ;',
  'cv.keep': 'les titres, pré-gaps, index, ISRC et MCN sont repris dans le fichier CUE ; l’image FISO n’est pas modifiée.',
  'cv.consent': 'J’accepte la conversion (la haute résolution ne sera pas conservée dans le BIN/CUE).',
  'cv.dest': 'Dossier de destination',
  'cv.go': 'Convertir',
  'cv.exists': 'Un BIN/CUE de cet album existe déjà dans ce dossier.',
  'cv.replace': 'Remplacer',
  'cv.done': 'Image CD audio créée',
  'cv.done.d':
    'Ouvrez le fichier .cue avec votre logiciel de gravure (par exemple Burn, Toast, ImgBurn ou cdrdao) et gravez en mode Disc-at-Once (DAO).',

  'job.convert': 'Conversion au format CD',
  'job.cd_prepare': 'Préparation de l’image CD',
  'job.cd_write': 'Écriture du BIN/CUE',

  'e.needs_conversion':
    'Cette image n’est pas au format CD audio (44,1 kHz, 16 bits, stéréo) : la conversion doit être acceptée.',
  'e.multichannel': 'Cette image a plus de 2 canaux : un CD audio est stéréo et FISO ne fait pas de mixage.',

  'w.aligned': 'Débuts de pistes alignés sur les secteurs du CD (déplacement de moins de 7 ms, aucun son ajouté ni retiré).',
  'w.end_padding': 'Quelques millisecondes de silence complètent la fin du disque.',
  'w.converted': 'L’audio a été converti au format CD (44,1 kHz / 16 bits / stéréo, avec dither).',
  'w.data_tracks':
    'Le disque contient aussi des pistes de données : seules les pistes audio sont conservées (la table des matières recense les autres).',
  'w.htoa_unreadable': 'Le lecteur ne peut pas lire l’audio placé avant la piste 1 (piste cachée) : il n’a pas été conservé.',
  'w.no_subchannel': 'Le lecteur ne fournit pas les données de sous-canal : les pré-gaps (INDEX 00) n’ont pas pu être détectés.',
  'w.read_errors': 'Certains secteurs n’ont pas pu être lus de façon fiable : l’extraction n’est PAS exempte d’erreurs.',
  'e.not_cdda': 'AccurateRip ne peut vérifier qu’une image extraite d’un CD audio.',

  'cr.clear': 'Tout supprimer',
  'cr.clear.q': 'Retirer les {n} fichiers de la liste ?',

  'cd.drive': 'Lecteur',
  'cd.refresh': 'Actualiser',
  'cd.eject': 'Éjecter',
  'cd.nodrive': 'Aucun lecteur optique détecté. Branchez un lecteur puis actualisez.',
  'cd.nodisc': 'Insérez un CD audio dans le lecteur.',
  'cd.reading': 'Lecture de la table des matières…',
  'cd.summary': '{n} pistes audio · {d}',
  'cd.data':
    'Ce disque contient aussi des pistes de données : seules les pistes audio seront conservées (la table des matières les mentionne).',
  'cd.htoa': 'Audio caché avant la piste 1 : {d} (sera conservé si le lecteur peut le lire)',
  'cd.cdtext': 'CD-Text lu sur le disque',
  'cd.mode': 'Mode d’extraction',
  'cd.secure': 'Sécurisé (recommandé)',
  'cd.secure.d':
    'Chaque secteur est lu deux fois, et relu jusqu’à obtenir deux lectures identiques en cas de différence ou d’erreur C2.',
  'cd.fast': 'Rapide',
  'cd.fast.d': 'Une seule lecture ; seuls les secteurs signalés en erreur sont relus.',
  'cd.offset': 'Décalage de lecture du lecteur',
  'cd.offset.d':
    'En échantillons. Corrige le décalage propre à chaque modèle de lecteur ; nécessaire pour la vérification AccurateRip.',
  'cd.offset.find': 'Chercher dans AccurateRip',
  'cd.offset.found': 'Décalage connu pour « {n} » : {o} ({s} envois). Appliqué.',
  'cd.offset.none': 'Ce lecteur n’est pas dans la base AccurateRip. Laissez 0 si vous ne connaissez pas son décalage.',
  'cd.ar': 'Vérifier avec AccurateRip après l’extraction',
  'cd.go': 'Extraire le CD',

  'job.rip': 'Lecture du CD',
  'job.pregaps': 'Détection des pré-gaps',
  'job.accuraterip': 'Calcul AccurateRip',

  'rip.title': 'CD extrait',
  'rip.errors.none': 'Aucune erreur de lecture',
  'rip.errors': '{n} secteur(s) illisible(s) : l’extraction n’est PAS parfaite',
  'rip.retried': '{n} secteur(s) relu(s)',
  'rip.c2': 'Pointeurs d’erreur C2 utilisés',
  'rip.noc2': 'Lecteur sans pointeurs C2',

  'ar.notrip': 'AccurateRip ne peut vérifier que les images extraites d’un CD audio.',
  'ar.title': 'AccurateRip',
  'ar.check': 'Vérifier avec AccurateRip',
  'ar.ok': 'Rip vérifié : toutes les pistes correspondent (confiance minimale {c}).',
  'ar.partial': 'Certaines pistes ne correspondent pas à la base AccurateRip.',
  'ar.absent': 'Ce disque n’est pas (encore) dans la base AccurateRip : aucune vérification n’est possible.',
  'ar.match': 'correspond (confiance {c})',
  'ar.nomatch': 'ne correspond pas',
  'ar.marked': 'L’état « Rip vérifié » a été enregistré dans l’image.',

  'mb.tracks': '{n} pistes',
  'mb.discs': '{d} CD · {t} pistes',
  'mb.fits': 'même nombre de pistes',

  'set.ar': 'Vérification AccurateRip',
  'set.ar.d': 'Compare automatiquement les extractions à la base AccurateRip (accès Internet).',
  'set.reader': 'Lecteur CD par défaut',
  'set.auto': 'Automatique',
  'set.thumbs': 'Icônes des fichiers .fiso',
  'set.thumbs.d':
    'Le Finder affiche la pochette avant comme icône des images FISO (extension Quick Look incluse dans l’application).',

  'eject.done': 'Disque éjecté.',

  detail: 'Détail technique',

  'e.no_media': 'Aucun disque dans le lecteur.',
  'e.not_cd': 'Le disque inséré n’est pas un CD.',
  'e.permission': 'macOS a refusé l’accès au lecteur.',
  'e.busy': 'Le disque est utilisé par une autre application (Musique, Finder…).',
  'e.platform': 'Cette fonction n’est disponible que sur macOS dans la version 1.0.',
  'e.no_audio': 'Ce disque ne contient aucune piste audio.',
  'e.mixed_layout': 'Ce disque mélange pistes audio et données d’une façon non prise en charge.',
  'e.no_discrecording': 'Le service des lecteurs optiques de macOS (DiscRecording) est indisponible.',
  'e.no_device': 'Le lecteur choisi n’est plus disponible.',
  'e.mixed_mode': 'AccurateRip ne gère pas les disques commençant par une piste de données.',

  sep: ' : ',

  'u.bytes': 'o,Ko,Mo,Go',

  'ch.stereo': 'stéréo',
  'ch.mono': 'mono',
  'ch.n': '{n} canaux',

  'pl.failed': 'Lecture impossible :',

  'cd.media.none': 'pas de disque',
  'cd.media.blank': 'disque vierge',

  'cr.cue.data': 'Ce CUE contient des pistes de données : FISO v1.0 ne stocke que l’audio et refusera ce disque.',

  'about.ref': 'Implémentation de référence du format ouvert FISO (Flac ISO) v1.0.',
  'about.data': 'Données MusicBrainz (CC0) · Pochettes : Cover Art Archive, droits de leurs auteurs respectifs.',

  'set.thumbs.fix': 'Réactiver les icônes',
  'set.thumbs.working': 'Enregistrement de l’extension…',
  'set.thumbs.ok': 'Extension active. Rouvrez la fenêtre du Finder pour voir les pochettes.',
  'set.thumbs.bad': 'macOS n’a pas activé l’extension.',
  'set.thumbs.quarantine': 'L’application est encore marquée « téléchargée » par macOS : voir l’aide.',
  'set.thumbs.dev': 'Disponible uniquement dans l’application installée sur macOS.',
};
window.LIMITS.fr = [
  'Lecture de CD : macOS uniquement dans la version 1.0 (Windows et Linux viendront ensuite).',
  'Détection des index : INDEX 00 (pré-gaps) et INDEX 01 ; les index 02 et plus ne sont pas recherchés lors de l’extraction d’un CD (ils sont conservés depuis un fichier CUE).',
  'Les données des sous-canaux (au-delà des index, ISRC et MCN) et les pistes de données des CD mixtes ne sont pas archivées.',
  'Le décalage de lecture n’est pas mesuré automatiquement : il vient de la base AccurateRip ou de votre saisie. Les échantillons hors de portée du lecteur sont remplacés par du silence (lecture au-delà du lead-out non utilisée).',
  'Lecture intégrée : aperçu en 16 bits pour les images haute résolution (l’image n’est jamais modifiée).',
  'Application signée en mode « ad hoc », sans identifiant Apple Developer : macOS demande une confirmation au premier lancement.',
];
window.HELP.fr = `
<h3>Créer une image</h3>
<p>Choisissez une source (fichiers FLAC/WAV, une piste par fichier, ou un fichier .cue), complétez les informations — au besoin avec MusicBrainz —, indiquez le numéro du disque et le nombre total de disques, puis choisissez l’emplacement. L’audio est compressé en FLAC niveau 8, vérifié pendant l’encodage, puis l’image entière est relue et contrôlée.</p>
<h3>Albums de plusieurs disques</h3>
<p>Une image correspond à un disque physique. Pour le disque 2, utilisez « Associer à un disque existant » et choisissez l’image du disque 1 : l’album partage alors le même identifiant et FISO Explorer regroupe automatiquement les disques présents dans le même dossier.</p>
<h3>Les trois états d’intégrité</h3>
<p><b>Fichier valide</b> : la structure est conforme. <b>Audio intègre</b> : l’audio décodé correspond exactement aux empreintes enregistrées. <b>Rip vérifié</b> : le contenu a été comparé à une référence externe. Ces états sont indépendants.</p>
<h3>Pré-gaps</h3>
<p>Le pré-gap (INDEX 00) d’une piste est stocké à la fin de la piste précédente, comme sur la chronologie du disque. Les transitions restent ainsi sans coupure et chaque piste s’extrait en FLAC sans réencodage.</p>
<h3>Extraire un CD audio</h3>
<p>Dans « Créer », choisissez « CD audio ». FISO Explorer lit la table des matières, le CD-Text, les codes ISRC et MCN, puis interroge MusicBrainz. Le mode sécurisé relit chaque secteur jusqu’à obtenir deux lectures identiques ; les secteurs qui restent douteux sont listés dans le rapport, jamais masqués. Renseignez le décalage de lecture de votre lecteur (bouton « Chercher dans AccurateRip ») pour que la vérification AccurateRip soit possible.</p>
<h3>Graver un CD</h3>
<p>Une image FISO est une archive : elle conserve l’audio exactement tel qu’il a été créé, même haute résolution. Pour graver un CD, utilisez « Convertir en image CD audio gravable » : FISO Explorer crée une image BIN/CUE au format CD audio (pistes placées sur les secteurs du CD, conversion seulement si vous l’acceptez), que vous gravez ensuite avec votre logiciel de gravure habituel, en mode Disc-at-Once (DAO). Les titres, pré-gaps, index, ISRC et MCN sont repris dans le fichier CUE ; l’image FISO n’est jamais modifiée.</p>
<h3>Icônes dans le Finder</h3>
<p>FISO Explorer installe une extension Quick Look : les fichiers .fiso affichent la pochette avant de leur album. Si les icônes n’apparaissent pas, cliquez sur « Réactiver les icônes » dans les Paramètres puis rouvrez la fenêtre du Finder. Si l’application a été téléchargée, macOS peut bloquer son extension tant qu’elle est marquée « téléchargée » : dans le Terminal, exécutez <code>xattr -dr com.apple.quarantine "/Applications/FISO Explorer.app"</code>, relancez FISO Explorer puis réactivez les icônes.</p>`;
