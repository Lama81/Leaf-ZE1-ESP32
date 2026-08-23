# Carnet de bord : Leaf Remote

Journal du projet : comment on est passé d'un ESP32 nu à un système complet de contrôle à distance de la Leaf (verrouillage, climatisation, monitoring) via une app installée sur téléphone, protégée par login Google, sans coût cellulaire hors action explicite.

Ce document est le récit détaillé. Pour la doc technique de référence (architecture, mutex, guards), voir [`CLAUDE.md`](CLAUDE.md). Pour l'intro publique du projet, voir [`README.md`](README.md).

---

## Partie 1 : Montage physique

### Matériel

- ESP32 (module WROVER-IE)
- Module Particle Boron (LTE)
- 2x transceiver CAN SN65HVD230, un par bus (EV-CAN, CAR-CAN)
- Buck 12V → 5V vers VIN de l'ESP32 ; la sortie 3V3 de l'ESP32 alimente les deux SN65HVD230
- Point de branchement sur le bus CAN de la voiture : connecteur gateway M101 (voir plus bas)
- Boîtier/enclosure (à compléter)
- Outils de soudure utilisés (à compléter)

### Câblage GPIO (connu, extrait du firmware)

| Fonction | GPIO ESP32 | Côté Boron | Notes |
|---|---|---|---|
| EV-CAN TX | GPIO32 | n/a | Écoute seule, toujours actif. Résistance pull-up 10kOhm vers 3.3V |
| EV-CAN RX | GPIO33 | n/a | |
| CAR-CAN TX | GPIO26 | n/a | Transmission ponctuelle (wake-up, lock/unlock, climat). Résistance pull-up 10kOhm vers 3.3V |
| CAR-CAN RX | GPIO14 | n/a | |
| UART TX → | GPIO19 | RX (D10) | Liaison ESP32 ↔ Boron, 9600 bauds |
| UART RX ← | GPIO21 | TX (D9) | |
| Réveil (EXT0) ← | GPIO34 | D8 | Niveau haut = réveil deep sleep. Résistance pull-down externe nécessaire (GPIO34-39 sans pull interne sur l'ESP32 d'origine) |
| GND commun | n/a | GND | Obligatoire entre ESP32 et Boron. Un seul point GND commun (buck + ESP32 + les deux SN65HVD230), relié à la masse châssis par un seul fil, pour éviter les boucles de masse |

Convention de couleur de fil utilisée sur le montage de référence : bleu = TX (peu importe le bus), jaune = RX. La gaine logique ESP32↔SN65 (6 fils : 3.3V, GND, TX1, RX1, TX2, RX2) peut être regroupée sans souci (non différentielle) ; CAN-H/L doivent rester torsadés et physiquement séparés de cette gaine logique.

### Étapes de montage (à compléter)

1. ?
2. ?
3. ?

### Point de branchement sur le véhicule

- Bus tappé : EV-CAN et CAR-CAN (deux bus logiques séparés de la Leaf ZE1)
- Emplacement physique du tap (sur cette ZE1 2019, 40 kWh) : connecteur gateway **M101**. EV-CAN sur les pins 12 (H) / 24 (L), CAR-CAN sur les pins 1 (H) / 13 (L). Le brochage exact varie selon l'année-modèle/finition, à revérifier au cas par cas.
- Alimentation 12V tirée depuis : à compléter
- Fusible ajouté : à compléter
- TCU d'origine : mort, but du projet est de le remplacer. CAN-H et CAN-L du TCU coupés/débranchés (confirmé) pour qu'il n'interfère plus sur le bus, laissé alimenté par ailleurs (Bluetooth mains-libres toujours fonctionnel).

---

## Partie 2 : Historique logiciel

### 2.1 Firmware ESP32 (`firmware/leaf-fw/main/main.c`)

Fichier unique, ESP-IDF, cible `esp32`. Grandes étapes (voir historique git pour le détail commit par commit) :

- **Verrouillage/déverrouillage** sur CAR-CAN, objectif principal, fiabilisé en premier.
- **Climatisation** : guards contre le redémarrage spontané du BCM (~30 min après extinction du véhicule). Trame de clôture `AUTO_DISABLE_CLIMATE_CONTROL` (`0x56E`, `46 08 32 00`) confirmée comme le vrai fix sur le terrain, envoyée systématiquement après une séquence ON.
- **Décodage batterie** (SOC, SOH) depuis EV-CAN. Le décodage de l'état de charge (`0x1D4` byte 6) s'est avéré peu fiable (signalait "en charge" en continu peu importe l'état réel) ; une alternative via les relais AC/QC (`0x390`) a été tentée puis abandonnée (trame jamais vue sur ce bus), retiré de l'affichage plutôt que de montrer une info fausse.
- **Mutex TWAI** : un seul contrôleur TWAI physique multiplexé entre EV-CAN (écoute continue) et CAR-CAN (transmission ponctuelle) via `twai_mutex`. Un bug de famine du mutex (`can_task` ne laissait jamais de fenêtre pour qu'une commande lock/unlock l'obtienne) a été corrigé en ajoutant un délai explicite de passation de mutex et un flag `bus_switch_requested` pour faire reculer `can_task` avant même la prise de mutex.
- **Pont UART vers le Boron** : protocole texte simple (`LOCK`, `UNLOCK`, `HEAT <temp>`, `HEAT_OFF`, `STATUS`, `WIFI_ON`, `WIFI_OFF`, `PING`), une commande par ligne terminée par `\n`, réponse `OK`/`ERR`/`STATUS ...`/`PONG`.
- **Deep sleep** : ajouté pour limiter le drain de la batterie 12V du véhicule quand il reste parqué longtemps. Deux sources de réveil (`esp_sleep_enable_ext0_wakeup` sur GPIO34 + `esp_sleep_enable_timer_wakeup` 6h). Réveil timer = chemin silencieux (écoute EV-CAN 3s, persiste SOC/SOH en NVS, redort, jamais de WiFi/webserver). Réveil EXT0/boot normal = démarrage complet avec boucle d'inactivité (5 min sans requête HTTP/UART → retour en sommeil). `uart_task` démarre avant `wifi_init_softap()` pour ne pas perdre les octets envoyés par le Boron juste après un réveil.
- **Persistance NVS** : télémétrie (SOC/SOH/véhicule), préférence WiFi (survit maintenant aux cycles deep sleep ; avant, `wifi_init_softap()` rallumait toujours le WiFi au réveil peu importe un `WIFI_OFF` demandé avant de dormir), état climate (récupération après crash/brownout).

### 2.2 Boron (`boron/src/boron.cpp`)

Relais Particle Cloud (LTE) → ESP32 par UART (`Serial1`, D9/D10). Chaque `Particle.function()` envoie une commande texte à l'ESP32 et attend la réponse avant de répondre au cloud, pas de polling, pas de `Particle.publish()` superflu (contrainte de données cellulaires minimales), sauf `status` qui doit vraiment transmettre une donnée.

Ajout du **wake-pulse** (pin D8 → GPIO34 ESP32) : avant chaque commande UART, le Boron met D8 à HIGH, attend ~2s (le temps que l'ESP32 sorte du deep sleep et redémarre `uart_task`), envoie la commande, puis repasse D8 à LOW (sinon l'ESP32 ne peut plus jamais entrer en deep sleep : un GPIO de réveil laissé HIGH déclenche un réveil immédiat).

### 2.3 Webapp (`webapp/`)

Page statique (HTML/CSS/JS, aucun framework), parle directement à l'API cloud Particle depuis le navigateur du téléphone. Fonctionne n'importe où (LTE ou WiFi), pas besoin d'être sur le WiFi local de la voiture.

Fonctionnalités : verrouillage/déverrouillage, climatisation (slider de température), refresh de statut (SOC/SOH/climat/véhicule/WiFi), toggle WiFi de l'ESP32 (utile pour un flash OTA à distance).

Bugs notables trouvés et corrigés en cours de route :
- **Badge "En ligne/Hors ligne"** : le point coloré séparé du texte causait un bug d'affichage récurrent (mal positionné dans le texte). Remplacé par du texte coloré simple, plus robuste.
- **Race condition SSE** : le flux d'événements (`EventSource`) était ouvert avant l'appel à la fonction cloud, mais sans attendre que l'abonnement soit réellement établi. Corrigé en attendant `onopen` avant de déclencher l'appel.
- **Authentification SSE cassée** : Particle a déprécié l'authentification par `?access_token=` en query param (requiert désormais le header `Authorization`), ce qu'`EventSource` natif ne permet pas d'envoyer. Remplacé par une lecture manuelle du flux SSE via `fetch()` + `ReadableStream`, qui permet le header. Bug additionnel trouvé en même temps : le nom d'événement `boron/status` contient un `/` qui doit être encodé (`%2F`) dans le chemin de l'URL, sinon 404.
- **Statut WiFi jamais synchronisé** : le switch WiFi de la page ne reflétait que sa propre supposition locale (dernier clic), jamais l'état réel de l'ESP32. Corrigé en ajoutant un champ `wifi:` à la réponse `STATUS`, lu par la webapp à chaque refresh.
- **Texte "Chauffer"** renommé en "Démarrer" (icône thermomètre au lieu d'une flamme), inapproprié en été quand la clim refroidit plutôt que chauffe ; la voiture décide elle-même chaud/froid selon la température cible.

### 2.4 Installation en app (PWA puis APK natif)

**Étape 1 : PWA (Progressive Web App)** : ajout d'un `manifest.json`, des icônes (logo llama, plusieurs tailles/formats PNG+SVG), et d'un service worker minimal (`sw.js`, requis par Chrome/Android comme critère d'installabilité même sans usage offline réel). Objectif : que Chrome propose "Installer l'application" avec une vraie icône et un lancement plein écran (WebAPK).

**Étape 2 : APK natif (TWA)** : la page étant protégée par Cloudflare Access (voir Partie 3), les robots de Google (WebAPK minting) ne pouvaient pas atteindre le manifest/les icônes, bloqués par le mur de login au même titre qu'un visiteur anonyme. Contournement : construction d'un vrai fichier `.apk` en local via [Bubblewrap](https://github.com/GoogleChromeLabs/bubblewrap) (outil officiel Google pour empaqueter une PWA en TWA/Trusted Web Activity), avec toute la chaîne d'outils installée à partir de zéro sur la machine de dev (Node.js, JDK 17 Temurin, Android SDK command-line tools). L'icône a été récupérée via un petit serveur HTTP local plutôt que depuis le domaine protégé, pour contourner Access pendant le build.

Détails techniques du build : projet Android généré directement via l'API `@bubblewrap/core` (pas l'assistant interactif, qui plante en environnement non-TTY), clé de signature générée avec `keytool`, compilation Gradle (`assembleRelease`), `zipalign` + `apksigner` pour la signature finale.

Deux itérations sur l'icône :
- La première version avait des coins arrondis "cuits en dur" dans l'image, posés sur un fond blanc dans le calque adaptatif Android, ce qui laissait apparaître un carré blanc aux coins avant que le masque du launcher (cercle/squircle) recadre le tout. Corrigé avec une variante bord-à-bord (sans arrondi maison), laissant Android faire tout le découpage.
- Le llama a aussi été rapetissé dans le canevas (plus de marge) pour ne pas être coupé sur certains launchers.

### 2.5 Fallback "Custom Tabs" vs plein écran vrai

Premier build de l'APK : `fallbackType: "customtabs"`. La page s'ouvre dans un onglet Chrome avec une mince barre visible (pas de vrai plein écran), parce que la vérification de confiance (Digital Asset Links) échouait. Cloudflare Access bloquait aussi `/.well-known/assetlinks.json`, empêchant Chrome de vérifier que l'app et le site web sont "la même chose".

---

## Partie 3 : Accès (Cloudflare + Google)

### 3.1 Départ : Cloudflare Worker sur `workers.dev`

La webapp a d'abord été déployée sur un Cloudflare Worker à l'URL par défaut de type `*.workers.dev`, protégée par **Cloudflare Access** avec **Google** comme fournisseur d'identité (login requis avant même de voir la page).

Problème découvert : le type de destination Access "Workers" (utilisé pour un `*.workers.dev`) protège la ressource en bloc, sans granularité par chemin. Impossible de laisser passer juste le manifest/les icônes pour que Chrome puisse construire l'app installable, sans désactiver le login sur tout le reste.

### 3.2 Migration vers un sous-domaine géré

Solution : brancher le Worker sur un sous-domaine personnalisé déjà chez Cloudflare, plutôt que rester sur `workers.dev`. Étapes suivies :

1. **Route Cloudflare Worker** : sous-domaine choisi → Worker (créée depuis la zone du domaine → Workers Routes, pas depuis les réglages du Worker lui-même ; l'ajout direct depuis le Worker donnait une erreur "No zones match").
2. **Enregistrement DNS** : un `CNAME` vers le domaine, proxied (nuage orange). Une Route seule ne suffit pas, il faut qu'un enregistrement DNS existe pour que la requête atteigne l'edge Cloudflare.
3. **Nouvelle app Access** (type **"Public DNS"** cette fois, pas "Workers" ; c'est ce type qui supporte les chemins) : politique "Allow" avec connexion Google requise (login methods : Google, pas "accept all identity providers" pour éviter qu'un login par email/PIN par défaut contourne la vérification Google).
4. **App Access de bypass séparée** ("PWA Assets Bypass") : même domaine, 3 destinations (`assets/*`, `.well-known/*`, `sw.js`), politique **Bypass** / Everyone. Ça laisse passer uniquement ces fichiers sans login, le reste du sous-domaine reste protégé.
5. **Restructuration des fichiers webapp** : `manifest.json` + icônes déplacés dans `webapp/assets/`, création de `webapp/.well-known/assetlinks.json` (empreinte SHA-256 du certificat de signature de l'APK), regroupés pour rester sous la limite de 5 destinations par app Access, et pour que les 3 exceptions de chemin les couvrent toutes.
   - Piège trouvé en chemin : `start_url`/`scope` dans `manifest.json` sont relatifs à l'emplacement du fichier manifest lui-même. En le déplaçant dans `assets/`, il fallait les rendre absolus (`/index.html`, `/`) sinon Chrome cherchait la page au mauvais endroit.
6. **APK reconstruit** avec le nouveau `host` : cette fois `/.well-known/assetlinks.json` est atteignable (bypass Access), la vérification de confiance Digital Asset Links réussit, et l'app s'ouvre en **vrai plein écran** (fini la barre Custom Tabs).

### 3.3 Désactivation de l'ancienne app (sans tout supprimer)

Après la migration, l'ancienne app Access (protégeant l'URL `workers.dev` d'origine) a été neutralisée en supprimant seulement sa **politique** (pas l'app au complet). Sans politique, Access refuse tout le monde par défaut ("default-deny"), même avec un login Google valide. La config de l'app (nom, destination) reste disponible si jamais elle doit être réactivée, en ajoutant une nouvelle politique.

---

## État de sécurité (au moment de ce journal)

- Le repo reste **privé** pour l'instant, décision explicite de ne pas le rendre public tout de suite.
- **2026-08-22** : historique git entièrement réécrit (repartie d'un commit unique) pour effacer le mot de passe WiFi et le token Particle qui trainaient depuis le premier commit. Le mot de passe WiFi vit maintenant dans `firmware/leaf-fw/main/wifi_secrets.h` (ignoré par git, voir `wifi_secrets.h.example`), et le token Particle n'est plus codé en dur dans `webapp/index.html` (saisi une fois dans le panneau Configuration, stocké en localStorage).
- Voir [`README.md`](README.md#sécurité) pour la checklist générale.
