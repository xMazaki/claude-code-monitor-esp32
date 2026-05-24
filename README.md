# Claude Code Usage Monitor - ESP32 tactile

[![Platform](https://img.shields.io/badge/platform-ESP32-323330?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32)
[![Arduino IDE](https://img.shields.io/badge/Arduino_IDE-2.x-00979D?logo=arduino&logoColor=white)](https://www.arduino.cc/en/software)
[![TFT_eSPI](https://img.shields.io/badge/TFT__eSPI-Bodmer-orange)](https://github.com/Bodmer/TFT_eSPI)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

Un petit dashboard physique posé sur ton bureau qui affiche en temps réel ton usage
**Claude Code** : la fenêtre de session 5h et la limite hebdomadaire 7j. Touch-screen,
WiFi, mises à jour automatiques, panneau de configuration web, OTA, et UI aux
couleurs officielles d'Anthropic.

## Quick start

> Setup minimal en 5 minutes, le détail est dans la section [Installation](#installation).

1. Cloner ce repo, ouvrir `claude_monitor.ino` dans Arduino IDE 2.x
2. Installer le core **ESP32** (Espressif) + lib **ArduinoJson v7** + lib **TFT_eSPI**
3. Copier le `User_Setup.h` du repo dans `Documents/Arduino/libraries/TFT_eSPI/`
4. Mettre ton SSID/password WiFi en haut du `.ino`, flasher
5. (Recommandé) Déployer le proxy : `cd cloud-proxy && vercel --prod`
6. Au premier boot, l'écran affiche son IP. Ouvrir dans un navigateur, coller la
   sessionKey Claude (cookie `sk-ant-sid…` depuis `claude.ai`) + l'URL du proxy
7. Reboot → ton dashboard est live

## Fonctionnalités

- **Deux vues**, swipe gauche/droite pour basculer :
  - **Horloge** : grande heure NTP + barres de progression compactes en bas
  - **Jauges** : deux cercles "Session 5h" et "Semaine 7j" avec countdown live
- **Animation des jauges** au remplissage
- **Code couleur** automatique : vert / orange / rouge selon des seuils configurables
- **Flash visuel** : le cercle clignote une fois quand un seuil est dépassé
- **Heure locale** : conversion automatique UTC → ton fuseau (presets Europe/US/Asie/Pacifique)
- **Countdown live** : "dans 2h15" au lieu d'une heure absolue
- **Refresh manuel** par tap, **réglages on-device** par appui long
- **Portail web** (HTTP sur le port 80) : configurer la sessionKey, l'orgUuid,
  l'URL du proxy, les seuils, l'intervalle de refresh, la vue par défaut, le fuseau horaire, reboot
- **OTA** (Over-The-Air) : flasher de nouvelles versions sans câble USB depuis
  Arduino IDE
- **NVS** : tous les réglages survivent aux reboots
- **Logo officiel Claude** "sparkle" en bitmap monochrome

## Testé sur le matériel suivant

- Guition JC2432W328 (ESP32 + 2.8" ST7789 320x240 + CST820 touch)
- Câble USB-C **data**
- WiFi 2.4 GHz

Il devrait fonctionner sur d'autres cartes ESP32 + écran ST7789 320x240, à condition
d'adapter `User_Setup.h` et le mapping des pins du touch CST820.

### Impression du boîtier

Modèle imprimable utilisé pour le support :
**[Desk stand for xTouch using ESP32-2432S028 - MakerWorld](https://makerworld.com/fr/models/49607-desk-stand-for-xtouch-using-esp32-2432s028#profileId-51458)**

## Installation

### 1. Cloner le repo

```bash
git clone https://github.com/xMazaki/claude-code-monitor-esp32.git
cd claude-code-monitor-esp32
```

### 2. Arduino IDE

- Installer **Arduino IDE 2.x**
- Ajouter le support ESP32 :
  - `Fichier → Préférences → URL de gestionnaire de cartes supplémentaires` :
    `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
  - `Outils → Type de carte → Gestionnaire de cartes` → installer **esp32** par Espressif Systems

### 3. Librairies

Via `Outils → Gérer les bibliothèques…` :

| Lib | Auteur | Version |
|---|---|---|
| TFT_eSPI | Bodmer | dernière (≥ 2.5) |
| ArduinoJson | Benoit Blanchon | **v7.x** |

`WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`, `WebServer` et
`ArduinoOTA` sont fournis par le core ESP32, rien à installer.

### 4. Configurer TFT_eSPI

**Étape critique** : remplacer le `User_Setup.h` fourni par TFT_eSPI par
celui du repo (`User_Setup.h` à la racine).

Ce fichier est calibré pour la **Guition JC2432W328** utilisée dans la démo
(ST7789 240x320, SPI HSPI, backlight GPIO 27, pins MISO/MOSI/SCLK/CS/DC).
Si tu utilises une autre carte, regénère-le via le wizard TFT_eSPI ou
ajuste les `#define` à la main.

Chemin typique de la lib :
```
Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
```

Sans cette étape : écran blanc ou couleurs cassées.

### 5. Configurer le WiFi

Ouvrir `claude_monitor.ino` dans Arduino IDE, modifier en haut du fichier :

```cpp
const char* WIFI_SSID = "ton_wifi";
const char* WIFI_PASS = "ton_password";
```

**C'est tout.** La sessionKey Claude et tout le reste se configurent via
le portail web après le premier boot - rien d'autre à hardcoder.

### 6. Paramètres de la carte (Arduino IDE)

- Type de carte : **ESP32 Dev Module**
- Flash Size : **4MB (32Mb)**
- Partition Scheme : **Default 4MB with spiffs**
- Upload Speed : **115200**
- Port : le COM de ta carte

### 7. Flash

Brancher la carte, sélectionner le port, cliquer **Upload** (flèche →).

### 8. (Recommandé) Déployer un proxy Vercel

**TL;DR** : Cloudflare bloque souvent l'ESP32 sur claude.ai en direct. Un mini-proxy
Vercel (gratuit, ~50 lignes) contourne ça en 2 commandes.

#### Étapes

1. Créer un compte gratuit sur [vercel.com](https://vercel.com) (login GitHub possible)
2. Installer la CLI Vercel :
   ```bash
   npm i -g vercel
   ```
3. Déployer le proxy :
   ```bash
   cd cloud-proxy
   vercel --prod
   ```
4. Suivre le prompt :
   - Login (au premier lancement)
   - `Set up and deploy?` → `y`
   - `Which scope?` → ton compte
   - `Link to existing project?` → `n`
   - `Project name?` → ce que tu veux (ex. `claude-monitor-proxy`)
   - `Code directory?` → `./` (juste Entrée)
   - `Modify settings?` → `n`
5. Vercel affiche une URL type `https://claude-monitor-proxy-xxx.vercel.app`
6. **Tester** : ouvre `https://claude-monitor-proxy-xxx.vercel.app/ping` dans ton
   navigateur. Doit afficher `{"ok":true,"ts":...}`
7. Garde cette URL pour l'étape 9

#### Pourquoi un proxy ?

L'ESP32 peut appeler `claude.ai` en direct, mais selon ton FAI, Cloudflare détecte
le fingerprint TLS de mbedTLS et ferme la connexion (`connection refused`). Le proxy
relaie les requêtes via le TLS Fastly de Vercel, qui passe à coup sûr.

Le proxy tourne 24/7 chez Vercel sur le free tier — **ton PC peut être éteint**.
La sessionKey transite via le header `X-Session-Key`. Le code (dans `cloud-proxy/api/proxy.js`,
~50 lignes) ne logge ni ne stocke rien — déploie le tien pour que la sessionKey ne
passe que par ton infra.

#### Tu peux skipper ?

Oui, le proxy est optionnel. Laisse simplement le champ "URL du proxy" vide à
l'étape 9. Si tu vois `Erreur API: -1 / connection refused` dans le moniteur série,
c'est que ton FAI est concerné — déploie le proxy.

### 9. Premier boot - configuration

L'écran affiche :
```
Configuration requise
Configurer via http://192.168.1.XX/
```

1. Ouvrir cette URL dans ton navigateur
2. Coller ta **sessionKey** Claude (voir section ci-dessous)
3. (Recommandé) Coller l'**URL du proxy** Vercel obtenue à l'étape 8. Laisser
   vide pour tenter en direct sur claude.ai — ça marchera ou pas selon ton FAI.
4. (Optionnel) Choisir un fuseau horaire, des seuils, la vue par défaut
5. Cliquer **Enregistrer**
6. Cliquer **Reboot** (ou taper sur l'écran pour forcer un refresh)

L'écran affichera désormais ton usage à chaque démarrage automatiquement.

## Récupérer ta sessionKey Claude

1. Connecte-toi sur https://claude.ai dans Chrome
2. Ouvrir les **DevTools** (`F12`)
3. Onglet **Application**
4. Sélectionner **Cookies → https://claude.ai**
5. Trouver la ligne `sessionKey`
6. Copier la valeur entière (commence par `sk-ant-sid…`, ~150 caractères)
7. La coller dans le champ "SessionKey" du portail web

⚠️ **Ne partage jamais cette clé.** Elle donne accès en lecture à ton compte
Claude. Elle expire de temps en temps (semaines/mois) - quand l'écran affiche
`Erreur API: 401`, il suffit d'aller en regénérer une.

## Utilisation au quotidien

### Interactions tactiles

| Geste | Effet |
|---|---|
| Tap court | Refresh manuel |
| Long press (~0.8s) | Écran réglages on-device |
| Swipe gauche/droite | Bascule horloge ↔ jauges |

### Portail web

`http://<ip-de-la-carte>/` (l'IP est affichée en bas à droite de l'écran réglages
ou tu peux la trouver dans ton routeur).

Tu peux y :
- Modifier ta sessionKey ou l'org UUID
- Coller / changer l'URL du proxy Vercel
- Changer l'intervalle de refresh (1 à 60 min)
- Définir la vue par défaut au boot
- Choisir un fuseau horaire (presets Europe/US/Asie + champ POSIX TZ avancé)
- Ajuster les 4 seuils de couleur (orange/rouge pour 5h et 7j)
- Forcer un fetch immédiat ou rebooter
- Accéder à `/usage.json` pour intégrer dans Home Assistant, scripts, etc.

### OTA - Mise à jour sans câble

Une fois la carte branchée et configurée :

1. Dans Arduino IDE, attendre quelques secondes
2. `Outils → Port` : **un port "network" apparaît** (`claude-monitor at 192.168.1.XX`)
3. Le sélectionner à la place du COM
4. Cliquer **Upload** normalement

L'écran affiche une barre de progression, puis l'ESP32 reboot avec la nouvelle version.

## Architecture / sécurité

- Flux par défaut (recommandé) : `ESP32 → Vercel proxy (cloud-proxy/) → claude.ai`.
  Le proxy est un simple relai stateless qui ajoute le cookie `sessionKey` reçu
  via `X-Session-Key` et transmet à claude.ai.
- Flux direct (sans proxy) : `ESP32 → claude.ai` directement. Marche
  uniquement si ton FAI / ton réseau n'est pas filtré par le Cloudflare bot
  protection. Quand ça ne marche pas, c'est un `connection refused` au
  handshake TLS. Vercel utilise Fastly TLS qui passe systématiquement.
- `WiFiClientSecure::setInsecure()` côté ESP32 : pas de validation de chaîne
  de certificats. La sessionKey étant elle-même un token de session, l'enjeu
  d'un MITM sur ton WiFi local est limité — mais une racine ISRG X1 pinning
  est possible si tu veux durcir.
- La sessionKey est stockée en NVS (flash interne, partition `nvs`).
  **Elle n'est pas chiffrée** — accessible par n'importe qui ayant
  physiquement la carte + un dumper de flash. Considère la carte comme un
  cookie d'auth physique.
- Le portail web utilise HTTP Basic Auth (`WEB_USERNAME` / `WEB_PASSWORD`
  dans le code). Laisser `WEB_PASSWORD = ""` pour désactiver l'auth.
- Le code du proxy est dans `cloud-proxy/api/proxy.js` (~50 lignes). Il ne
  logge ni ne stocke rien — déploie le tien pour que la sessionKey ne
  passe que par ton infra.

## Dépannage

### Compilation : `ArduinoJson.h: No such file or directory`
La lib n'est pas installée. Voir Étape 3, prendre **ArduinoJson v7.x** par
**Benoit Blanchon** (pas "Arduino_JSON" qui est une autre lib).

### Upload : `Failed to connect / Invalid head of packet / serial corruption`
- Baisser **Upload Speed à 115200** dans Outils
- Changer de câble USB : beaucoup de câbles vendus avec les cartes sont
  charge-only. Un câble de téléphone récent marche
- Brancher en USB direct sur la carte mère, pas via un hub
- Si vraiment rien : maintenir **BOOT** pendant que tu cliques Upload,
  relâcher 1-2s après que le `Connecting...` démarre

### Upload : `Could not open COM4, port is busy`
Le moniteur série est ouvert et bloque le port. Le fermer, retenter.

### Écran blanc / couleurs cassées
Le `User_Setup.h` de TFT_eSPI n'a pas été remplacé. Voir Étape 4.

### "Erreur API: 401"
La sessionKey a expiré. Récupérer une nouvelle valeur (voir section sessionKey)
et la coller dans le portail web → Enregistrer → Reboot.

### "Erreur API: -1" ou "connection refused"
L'ESP32 n'arrive pas à joindre l'API. Causes possibles :
- **Tu es en mode direct (champ "URL du proxy" vide) et Cloudflare bloque
  ton ESP32** → suivre l'étape 8 pour déployer le proxy Vercel et coller
  l'URL dans le portail.
- **L'URL du proxy a une typo ou un slash à la fin** → vérifier que
  `https://xxx.vercel.app/ping` renvoie bien `{"ok":true,...}` dans le navigateur.
- **Vercel est temporairement down** → vérifier sur [vercel-status.com](https://www.vercel-status.com).
- **Heap fragmenté** (`SSL - Memory allocation failed` dans le log série) →
  déjà géré par le code (les sprites sont libérés temporairement pendant
  le fetch). Si tu vois encore l'erreur, baisser la taille des sprites
  dans `setup()`.

### "Erreur API: 502"
Le proxy a répondu mais claude.ai a planté ou la sessionKey est invalide.
Vérifier la sessionKey dans le portail.

### Le tactile est imprécis ou ne répond pas
Le CST820 utilise I2C sur GPIO 33/32. Vérifier que rien d'autre n'utilise ces
pins. La calibration est dans `touchRead()` pour la rotation 1 - adapter si
ton écran est dans une autre orientation.

### L'heure est fausse de plusieurs heures
Le fuseau horaire n'est pas le bon. Aller dans le portail web → "Fuseau
horaire" → choisir le bon preset → Enregistrer.

### Port OTA pas visible dans Arduino IDE
- Attendre 10-30s après le boot de la carte (mDNS prend un moment)
- Vérifier que ton PC et l'ESP32 sont sur le même réseau WiFi
- Sur Windows : installer **Bonjour Print Services** (Apple) pour le support mDNS

## Crédits

- Bypass Cloudflare / proxy Vercel pattern :
  [ClaudeGauge](https://github.com/dorofino/ClaudeGauge)
- Logo "sparkle" : Anthropic, via Wikimedia Commons
- TFT_eSPI : [Bodmer](https://github.com/Bodmer/TFT_eSPI)

## Licence

MIT - fais-en ce que tu veux. Les marques "Claude" et "Anthropic" appartiennent
à Anthropic PBC. Ce projet n'est pas affilié à Anthropic.

## Contribuer

Issues et PR bienvenues. Idées de features potentielles dans le code mais pas
implémentées : countdown vocal/buzzer, mode veille auto avec dim du backlight,
breakdown par modèle (Opus/Sonnet/Haiku), historique 24h en graphique sparkline.
