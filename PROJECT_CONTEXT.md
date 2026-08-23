# Nissan Leaf ZE1 ESP32 CAN Project Context

Ce fichier est la mémoire de projet portable entre les deux ordinateurs de
l'utilisateur (la mémoire de session de Claude Code reste locale à une seule
machine, pas dans git). Le repo est privé donc rien n'empêche d'y mettre le
statut détaillé. **À tenir à jour après chaque changement significatif**, pas
seulement en fin de session.

## Situation actuelle (2026-07-31)

- v1.0.0 stable. **LOCK/UNLOCK fiable sur CAR-CAN**, objectif principal atteint.
- Climate/HVAC fonctionne, considéré secondaire, guards anti-redémarrage
  spontané en place (voir plus bas).
- Travail en cours : fiabiliser l'affichage de l'état de charge, ajouter la
  tension 12V, préparer la connectivité Boron LTE.

---

## Git

Branche active : **main**, tout le développement se fait ici directement,
pas de branche de feature divergente actuellement (les autres branches ci-
dessous sont des points de repère historiques, pas du travail parallèle).

Dernier commit : `647159e` (poussé ? vérifier `git status`, souvent en
avance sur `origin/main`, ne pas assumer que c'est déjà sur GitHub).

Tags : `v0.1-fonctionnel`, `v1.0-working`, `v1.0.0`, `v1.1-stable`

Autres branches : `boron-lte`, `can-experiments`, `stable-baseline` (snapshots,
pas de merge en attente).

---

## Firmware actuel

Fichier unique : `firmware/leaf-fw/main/main.c` (~870 lignes, un seul
composant ESP-IDF). Détails d'architecture (dual CAN bus, mutex, guards
climate, decodage CAN) : voir `CLAUDE.md` à la racine du repo, tenu à jour et
plus détaillé que ce fichier pour l'architecture technique.

---

## Statut des fonctionnalités

### LOCK / UNLOCK
Fonctionnel, fiable. `do_lock_sequence(bool lock)`, wakeup 0x68C/0x56E puis
commande répétée sur CAR-CAN.

### Climate / HVAC (secondaire)
Fonctionne. Bug connu du BCM : redémarrage spontané de la clim ~30 min après
l'arrêt du véhicule. Deux guards défensives autour de `g_climate_active` :
- **Guard 1/1b** (event-driven) : transition ON->OFF sur `0x11A`, plus
  détection de silence >3s sur `0x11A` (le bus peut simplement arrêter
  d'émettre à l'arrêt du véhicule plutôt que de publier un OFF explicite).
- **Guard 2** (watchdog) : timer 20 min, force l'OFF si Guard 1/1b n'a pas
  fired.
Committé dans `318b5e5`. **Guard 1b pas confirmé testé en usage réel** au
27/07, à vérifier où ça en est avant de considérer le bug réglé.
Piste non implémentée si le bug persiste : OVMS envoie une 3e trame
`{0x46,0x08,0x32,0x00}` sur `0x56E` ~1s après la rafale ENABLE, qu'on
n'envoie pas actuellement, candidate si Guard 1b ne suffit pas.

### Décodage de l'état de charge (EV-CAN)
Ancien : `0x1D4` byte 6 utilisé comme source primaire, **peu fiable**
(affichait "En charge" en continu peu importe l'état réel). Cause identifiée
via le code source OVMS : `0x1D4` n'y est qu'un signal secondaire (détection
d'une interruption), jamais utilisé pour déterminer l'état initial.

Nouveau (implémenté dans le working tree, **pas encore commité ni testé sur
véhicule** au 31/07) : décodage via `0x390` (relais AC/QC), confirmé sur
EV-CAN par le code source OVMS (`IncomingFrameCan1`, commentaire "CAN1 is
connected to EV-CAN") :
```c
bool ac_state = (data[3] & 0x20) == 0x20;  // charge normale (AC)
bool qc_state = (data[4] & 0x40) == 0x40;  // charge rapide (QC/CHAdeMO)
bool en_charge = ac_state || qc_state;
```
`0x1D4` gardé en log sous `ChargeRaw:0x%02X`, ne pilote plus le dashboard.

**Prochaine étape : `idf.py build`, flasher, tester une vraie charge,
confirmer, puis commit.**

### Dashboard web
Cartes GIDS et PACK retirées (peu utiles). `FW_VERSION` maintenant
auto-généré (`__DATE__ " " __TIME__`), toujours affiché en haut de la page,
ne jamais revenir à une chaîne codée en dur.

### Tension 12V (demandé, pas commencé)
Aucun décodage encore identifié, reste à trouver la trame/l'offset CAN
source pour la tension de la batterie 12V auxiliaire.

### Connectivité Boron LTE (planifié, pas de code)
Pins UART décidées pour relier un module Particle Boron (LTE) à l'ESP32 :
- ESP32 GPIO19 → Boron RX (D10)
- ESP32 GPIO21 ← Boron TX (D9)
- GND commun entre les deux
- Wake ESP32 GPIO34 → Boron D6

La branche `boron-lte` existe mais ne contient pas encore de code lié.

---

## Méthode de travail

- Ne jamais tout réécrire d'un coup, une modification ciblée à la fois.
- Cycle : modifier → `idf.py build` → tester sur le vrai véhicule (pas
  d'accès USB pendant la conduite, diagnostics via le monitor web/SSE ou
  des endpoints HTTP comme `/status`) → commit.
- Avant une grosse modification : `git add` + `git commit` d'abord pour
  isoler l'état stable précédent.

---

## Prochaine action

1. `idf.py build` + tester le fix de charge `0x390` sur une vraie charge.
2. Commit une fois confirmé stable.
3. Ensuite : tension 12V, puis éventuellement la 3e trame climate OVMS si le
   bug de redémarrage revient.
