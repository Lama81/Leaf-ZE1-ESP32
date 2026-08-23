# Leaf-ZE1-ESP32

*[English version](README.md)*

Firmware DIY pour contrôler une Nissan Leaf ZE1 (2019+) à distance : verrouillage/déverrouillage des portes, préconditionnement climatisation, et monitoring batterie/véhicule en direct, soit en local via WiFi (à côté de la voiture), soit à distance depuis n'importe où via un module cellulaire LTE (Particle Boron) et une page web.

Projet personnel, non affilié à Nissan. Les trames CAN (wake-up, lock/unlock, décodage batterie) viennent de rétro-ingénierie (voir [Sources](#sources)) ; certaines sont confirmées sur le véhicule, d'autres sont du best-effort documenté dans le code.

## Fonctionnalités

- Verrouillage / déverrouillage des portes (fiable, objectif principal)
- Préconditionnement climatisation (température réglable), avec guards contre le redémarrage spontané du BCM
- Monitoring batterie (SOC/SOH), climatisation, état véhicule : dashboard web local en direct (SSE)
- Contrôle à distance via LTE (Boron + Particle Cloud), aucune app à installer, page web installable en PWA ou en APK natif (voir [Installation en app](#installation-en-app-pwa--apk))
- Deep sleep de l'ESP32 (réveil sur commande à distance ou toutes les 6h) pour limiter le drain de la batterie 12V du véhicule quand la voiture est parquée

## Architecture

```
Téléphone (webapp) --HTTPS--> Particle Cloud --LTE--> Boron --UART--> ESP32 --CAN--> Voiture
                                                                         |
Téléphone (WiFi local, leafcan) -------------------------------------- AP local
```

- **ESP32** : un seul TWAI (CAN) physique, multiplexé entre deux bus logiques de la voiture : EV-CAN (écoute passive continue : batterie, climat, état véhicule) et CAR-CAN (transmission ponctuelle : wake-up, lock/unlock, climat). Sert aussi un dashboard web local (AP WiFi `leafcan`) et de l'OTA.
- **Boron** (Particle, LTE) : relais entre le cloud Particle et l'ESP32 par UART. Chaque commande cloud (`lock`, `unlock`, `heat`, `status`, `wifi`) est transmise en texte simple et attend une réponse avant de répondre au cloud : pas de polling, aucune donnée cellulaire consommée en dehors d'une action explicite.
- **webapp** : page statique (HTML/CSS/JS, aucun framework) qui parle directement à l'API cloud Particle depuis le navigateur. Installable comme app (PWA) sur téléphone.

Détails d'architecture technique (mutex TWAI, guards climate, décodage des trames CAN, concurrence) : voir [`CLAUDE.md`](CLAUDE.md).

## Structure du repo

```
firmware/leaf-fw/   ESP32 - firmware ESP-IDF (fichier unique main/main.c)
boron/               Particle Boron - relais LTE (src/boron.cpp)
webapp/              Page web statique - dashboard + contrôle à distance
CLAUDE.md            Référence technique détaillée (architecture, guards, décodage CAN)
```

## Matériel

- **Carte de développement ESP32** (cible `esp32` simple, voir `sdkconfig` ; pas une variante S2/S3/C3). Testé sur un module WROVER-IE.
- **2x transceiver CAN SN65HVD230** (un par bus, logique 3.3V).
- **Particle Boron** (module cellulaire LTE, pour le contrôle et la télémétrie à distance au-delà de la portée WiFi locale).
- **Buck 12V vers 5V** vers le pin VIN de l'ESP32. La sortie 3V3 de l'ESP32 alimente les deux transceivers SN65HVD230. Un seul point GND commun (buck + ESP32 + les deux transceivers), relié à la masse châssis par un seul fil, pour éviter les boucles de masse.
- **Résistances pull-up 10kOhm** sur les lignes TX de CAR-CAN et EV-CAN (GPIO32 et GPIO26, chacune vers 3.3V) ; pas nécessaire sur RX, GND, ou VCC.
- **Point de branchement** : sur cette ZE1 (2019, 40 kWh), les deux bus ont été tapés au **connecteur gateway M101** : EV-CAN sur les pins 12 (H) / 24 (L), CAR-CAN sur les pins 1 (H) / 13 (L). Le brochage du connecteur et le point de tap exact varient selon l'année-modèle et la finition ; à vérifier sur ton propre véhicule avant de câbler.

Le boîtier et les étapes complètes de câblage/assemblage ne sont pas encore finalisées dans ce repo ; voir [`CARNET_DE_BORD.md`](CARNET_DE_BORD.md) pour ce qui a été journalisé jusqu'ici.

## Câblage / pins GPIO (ESP32)

| Fonction | GPIO ESP32 | Côté Boron | Notes |
|---|---|---|---|
| EV-CAN TX | GPIO32 | n/a | Mode écoute seule (listen-only), toujours actif |
| EV-CAN RX | GPIO33 | n/a | |
| CAR-CAN TX | GPIO26 | n/a | Mode normal, transmission ponctuelle uniquement |
| CAR-CAN RX | GPIO14 | n/a | |
| UART TX → | GPIO19 | RX (D10) | Liaison ESP32 ↔ Boron, 9600 bauds |
| UART RX ← | GPIO21 | TX (D9) | |
| Réveil (EXT0) ← | GPIO34 | D8 | Niveau haut = réveil deep sleep. **Nécessite une résistance pull-down externe** (GPIO34-39 n'ont pas de pull interne sur l'ESP32 d'origine) |
| GND commun | n/a | GND | Obligatoire entre ESP32 et Boron |

Un transceiver CAN (2x, un par bus) est nécessaire entre les GPIO TWAI de l'ESP32 et les bus CAN réels du véhicule, non documenté ici, à adapter selon le modèle utilisé.

## Build & flash

### ESP32 (firmware/leaf-fw)

Prérequis : toolchain ESP-IDF installée (cible `esp32`), `idf.py` accessible dans le shell.

```
cd firmware/leaf-fw
idf.py build
idf.py -p <PORT> flash monitor      # premier flash, par USB
```

Après le premier flash USB, les mises à jour suivantes peuvent se faire par OTA. Connecte-toi au WiFi `leafcan` et ouvre `http://192.168.4.1/update` pour uploader le `.bin` (`build/leaf-fw.bin`).

### Boron (boron/)

Prérequis : [Particle CLI](https://docs.particle.io/getting-started/developer-tools/cli/) installée, connectée à ton compte, device réclamé (claimed).

```
cd boron
particle cloud flash <device_id_ou_nom>     # compile dans le cloud Particle + flash OTA (LTE)
```

Ou en local par USB (ne consomme aucune donnée cellulaire) :
```
particle compile boron src --target 6.4.1 --saveTo target/6.4.1/boron/boron.bin
particle flash --usb target/6.4.1/boron/boron.bin
```

### webapp (webapp/)

Page statique, aucun build. Héberge le dossier tel quel sur n'importe quel service HTTPS (Cloudflare Pages/Workers, GitHub Pages, Netlify...). HTTPS est requis pour l'installation en PWA (icône sur écran d'accueil, mode plein écran).

## Installation en app (PWA ou APK)

La page est installable de deux façons sur Android :

- **PWA** : dans Chrome, menu (trois points) puis "Installer l'application". Nécessite `manifest.json` + un service worker (`sw.js`, déjà inclus) atteignables sans authentification. Si la page est derrière un mur de login (ex: Cloudflare Access), prévoir une exception pour ces fichiers spécifiquement, sinon Chrome ne peut pas construire l'app installable.
- **APK natif (TWA)** : pour un vrai plein écran sans dépendre du comportement PWA de Chrome, la page peut être empaquetée en `.apk` avec [Bubblewrap](https://github.com/GoogleChromeLabs/bubblewrap) (outil officiel Google). Ça demande Node.js, un JDK 17, et le Android SDK command-line tools. Le plein écran "de confiance" (sans barre du navigateur) exige aussi que `/.well-known/assetlinks.json` (empreinte SHA-256 du certificat de signature de l'app) soit atteignable sans authentification, même remarque que pour le manifest.

Voir [`CARNET_DE_BORD.md`](CARNET_DE_BORD.md) pour le détail du build APK et de la config d'accès utilisée sur le déploiement de référence.

## Configuration avant de flasher

- **WiFi local** : copie `firmware/leaf-fw/main/wifi_secrets.h.example` en `wifi_secrets.h` (ignoré par git) et renseigne ton propre `WIFI_SSID`/`WIFI_PASS` avant de flasher.
- **APK natif (optionnel)** : si tu construis ta propre APK (voir [Installation en app](#installation-en-app-pwa--apk)), copie `webapp/.well-known/assetlinks.json.example` en `assetlinks.json` (ignoré par git) et renseigne ton propre `package_name`/`sha256_cert_fingerprints`.
- **Device ID / token Particle** : à entrer dans la page web, section Configuration (en bas). Stocké uniquement en `localStorage` sur chaque appareil, jamais dans le code source.
  - Crée un access token qui n'expire pas : `particle token create --never-expire` (les tokens par défaut expirent après 90 jours).

## Sécurité

- Si la webapp est hébergée quelque part de plus qu'un usage strictement privé, mettre une couche d'authentification devant (ex: Cloudflare Access) est recommandé. La page elle-même n'a aucune protection interne au-delà du token Particle.

## Deep sleep

L'ESP32 peut entrer en deep sleep après une période d'inactivité (pas de requête HTTP ni de commande UART) pour limiter la consommation quand la voiture est parquée longtemps. Deux sources de réveil : commande à distance via le Boron (GPIO34) ou un timer périodique (6h, rafraîchit silencieusement SOC/SOH sans jamais activer le WiFi). Le dashboard web local est injoignable tant que l'ESP32 dort, usage principal prévu via le Boron/la webapp, pas l'accès direct.

## Sources

Trames CAN et séquences de commande basées sur [OVMS `vehicle_nissanleaf.cpp`](https://github.com/openvehicles/Open-Vehicle-Monitoring-System-3) et le DBC [`dalathegreat/leaf_can_bus_messages`](https://github.com/dalathegreat/leaf_can_bus_messages). Référence brochage du connecteur gateway (M101) : [blog.jingo.uk](https://blog.jingo.uk). Documentation officielle OVMS ZE1 : [docs.openvehicles.com](https://docs.openvehicles.com).
