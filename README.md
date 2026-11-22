# Spa Balboa GL/EL – pilotage depuis le téléphone (version française)

Firmware ESP32 qui se branche sur la carte de commande d'un spa Balboa des séries GL/EL (GL2000, GL2001, GL8000, EL2000, EL8000…) par un port clavier principal libre, et qui fournit une page web pour piloter le spa depuis un téléphone, sans Home Assistant.

Ce dépôt est une **traduction française** du projet [zaiclopz/jacuzzi-balboa](https://github.com/zaiclopz/jacuzzi-balboa) (interface d'origine en roumain), lui-même basé sur [netmindz/balboa_GL_ML_spa_control](https://github.com/netmindz/balboa_GL_ML_spa_control) (décodage du protocole, communication RS485).

Changements par rapport à l'original :

- interface, journal, messages d'erreur et portail Wi-Fi traduits en français ;
- tuiles renommées selon l'équipement réel : **Pompe 1**, **Pompe 2**, **Blower**, **Éclairage** ;
- mises à jour en ligne tirées de **ce** dépôt (sinon une mise à jour réinstallerait la version roumaine).

## Ce que fait le module

- Page web pour téléphone : température de l'eau, consigne (+/−), chauffe, pompes 1 et 2, blower, éclairage, mode Standard / Économie / Veille.
- Température extérieure et météo pour la ville indiquée dans la page. C'est le téléphone qui la récupère auprès du service gratuit [Open-Meteo](https://open-meteo.com) (usage non commercial) : elle n'apparaît que si le téléphone a Internet.
- Codes d'erreur (facultatif, case « Codes d'erreur » dans Personnalisation, désactivée par défaut) : quand l'afficheur du spa montre un code (OHH, dr, SnA, LF…), il apparaît sous le graphique avec son explication. Les codes inconnus apparaissent tels quels. Le format exact des messages d'erreur sur le bus n'est pas confirmé sur toutes les cartes.
- Graphique de la température de l'eau sur 24 h (un point toutes les 5 minutes, perdu au redémarrage du module).
- Personnalisation depuis la page : nom affiché, ville pour la météo, nom et affichage de chaque fonction. Les réglages sont enregistrés dans le module et valent pour tous les téléphones.
- Icône sur l'écran du téléphone (Chrome : ⋮ → « Ajouter à l'écran d'accueil » ; Safari : Partager → « Sur l'écran d'accueil »).
- Mise à jour en temps réel (WebSocket), sans recharger la page.
- Configuration Wi-Fi depuis le téléphone, sans mot de passe dans le code.
- Home Assistant par MQTT, en option.
- Mise à jour du firmware par Wi-Fi.

## Matériel

- ESP32 DevKit (ESP-WROOM-32)
- Module TTL–RS485 à contrôle automatique de direction (broches TXD, RXD, VCC, GND, **sans** RE/DE)
- Abaisseur réglé sur 5 V (si l'ESP32 est alimenté par le spa) ou chargeur USB 5 V
- Connecteur Molex Micro-Fit 3.0, 8 broches (boîtier 43025-0800 + cosses femelles 43030)
- Fils Dupont femelles

## Branchements

Broches du connecteur de la carte, numérotées comme sur la photo du [wiki du projet d'origine](https://github.com/netmindz/balboa_GL_ML_spa_control/wiki/Install-Guide) (1–2 en haut, 7–8 en bas), sur un port « Main panel » libre (J70 à J73 sur un GL8000) :

| Carte du spa | Relié à |
|---|---|
| Broche 1 | RS485 B |
| Broche 3 | RS485 A |
| Broche 5 (sélection clavier) | ESP32 GPIO18 |
| Broche 6 (≈ 9,7 V) | Abaisseur IN+ (seulement si alimentation par le spa) |
| Broche 7 ou 8 (masse) | ESP32 GND et abaisseur IN− |
| Broches 2 et 4 | Non connectées |

| Module RS485 | ESP32 |
|---|---|
| RXD | GPIO19 |
| TXD | GPIO23 |
| VCC | 3V3 |
| GND | GND |

Abaisseur OUT+ → broche 5V de l'ESP32, OUT− → GND. Vérifiez au multimètre que la sortie est bien à 5 V avant de brancher l'ESP32. N'alimentez jamais l'ESP32 en même temps par l'abaisseur et par l'USB.

⚠️ **Mesurez la broche 5 au multimètre.** Elle est reliée directement à l'ESP32, qui accepte 3,3 V au maximum. Si elle est à 5 V, ajoutez un pont diviseur (10 kΩ en série, 20 kΩ vers la masse).

⚠️ **Coupez l'alimentation du spa au tableau avant tout branchement.**

## Installer le firmware

### Méthode simple (depuis le navigateur)

1. Ouvrez [Releases](../../releases) et téléchargez `jacuzzi-full-install.bin` (installation complète). `jacuzzi-update.bin` sert aux mises à jour par Wi-Fi. Sans version publiée, prenez l'artefact `jacuzzi-firmware` du dernier build réussi dans [Actions](../../actions) (conservé 90 jours).
2. Branchez l'ESP32 à l'ordinateur en USB et ouvrez dans Chrome ou Edge : https://espressif.github.io/esptool-js/
3. Cliquez sur **Connect** et choisissez le port de l'ESP32.
4. Dans **Flash Address**, saisissez `0x0`, choisissez `jacuzzi-full-install.bin`, puis cliquez sur **Program**.

### Méthode pour développer (PlatformIO)

```
cd sensor
pio run -e esp32dev -t upload
pio device monitor
```

La page web se modifie dans `sensor/web/index.html`. Le fichier `sensor/src/webui.h` est régénéré automatiquement à chaque compilation. Les icônes se refont avec `python sensor/web/make_icons.py` (nécessite Pillow).

## Premier démarrage

1. Alimentez l'ESP32. Sur le téléphone, le réseau Wi-Fi **Jacuzzi-Setup** apparaît : connectez-vous-y.
2. La page de configuration s'ouvre (sinon, allez sur `http://192.168.4.1`). Choisissez votre réseau et saisissez son mot de passe. Laissez les champs MQTT vides si vous n'utilisez pas Home Assistant.
3. L'ESP32 redémarre et rejoint votre réseau.
4. Page du spa : **http://jacuzzi.local** (ou l'adresse IP de l'ESP32 indiquée par la box).

Si le réseau enregistré est indisponible au démarrage, le point d'accès Jacuzzi-Setup reste ouvert 5 minutes, puis l'ESP32 redémarre et réessaie.

## Utilisation

- **Consigne :** appuyez sur + ou − ; la valeur est envoyée après 1,5 s sans appui. Si la consigne est « Inconnue », un appui la lit dans le spa.
- **Pompes, blower, éclairage :** chaque appui équivaut à la touche correspondante du clavier.
- **Mode :** Standard / Économie / Veille. Pour un spa peu utilisé : laissez-le en Économie et passez en Standard la veille du bain.
- **Réglages et informations** (en bas de page) : personnalisation, version du firmware, mise à jour, changement de réseau Wi-Fi.

## Vérifier les codes de votre clavier (premier test)

Les codes des touches viennent de claviers de GL2000. Avant de compter dessus :

1. Ouvrez **Journal (dépannage)** et cochez **Capturer les touches du clavier du spa**.
2. Appuyez sur une touche du clavier du spa (ML700…), puis notez-la avec **Ajouter la note**.
3. Le code apparaît avec la lettre **P**. Comparez-le à ceux de `sensor/src/constants.h`. Un bouton **Envoyer** permet de le rejouer pour tester.

## Mise à jour du firmware par Wi-Fi

**En ligne :** **Réglages et informations → Rechercher une mise à jour → Installer**. Le module télécharge la dernière version publiée dans les [Releases de ce dépôt](https://github.com/fquillerier/jacuzzi-balboa/releases), vérifie le fichier (SHA-256) et redémarre. Le téléphone doit avoir Internet. Historique des versions : [CHANGELOG.md](CHANGELOG.md).

**Manuelle :** **Réglages et informations → Mise à jour manuelle (fichier)** (ou `http://jacuzzi.local:8080/update`), puis envoyez `jacuzzi-update.bin`.

## Sécurité

Le module n'a **aucun mot de passe** (page, API, mise à jour manuelle sur le port 8080, telnet sur le port 23). N'importe quel appareil de votre réseau local peut donc le piloter ou y installer un autre firmware.

- N'ouvrez **jamais** de port vers le module sur votre box.
- Pour un accès hors de chez vous, passez par le **VPN de votre box** (WireGuard…).
- Idéalement, placez le module sur un réseau invité ou dédié aux objets connectés.

## API (pour les intégrations)

| Adresse | Rôle |
|---|---|
| `GET /api/status` | État en JSON |
| `GET /api/cmd?a=up\|down\|light\|jet1\|jet2\|aux\|mode\|time` | Un appui de touche |
| `GET /api/target?t=37.5` | Réglage de la consigne (26–40 °C, pas de 0,5) |
| `GET /api/mode?m=0\|1\|2` | Standard / Économie / Veille |
| `GET /api/config` / `POST /api/config` | Réglages de la page (nom, fonctions affichées) |
| `GET /api/history` | Température des dernières 24 h |
| `POST /api/wifireset` | Efface le réseau Wi-Fi et redémarre en mode configuration |
| WebSocket port 81 | État en temps réel |

## Limites connues

- Les commandes sont des appuis de touches simulés. L'état affiché est celui que renvoie la carte du spa : il change après confirmation par la carte.
- Les codes des pompes viennent du projet d'origine et y sont signalés comme non vérifiés sur tous les modèles.
- La **pompe 3** n'est pas gérée : son code de touche et son état ne sont pas connus (voir ci-dessous).
- L'état du blower n'est pas lu dans les messages de la carte : la tuile ne fait que basculer.
- La consigne n'est connue qu'après un passage de la carte dans le menu de réglage de la température.

## Équipements non gérés (pompe 3, ozone, filtration)

Le message d'état décodé ne contient que : température, pompes 1 et 2 (2 bits chacune), chauffe, éclairage, mode, heure et texte de l'afficheur. Pour ajouter un équipement, il faut :

1. **Son code de touche** : à capturer avec la fonction « Capturer les touches » ci-dessus (pour la pompe 3 : touche Jets 3 du clavier).
2. **Son état** : repérer quel octet du message change quand il démarre. Le journal affiche les messages bruts (lettre **R**) : notez l'heure, démarrez l'équipement, comparez.

L'ozonateur et les cycles de filtration ne se commandent pas depuis le clavier : ils sont gérés par la carte. Seul leur état pourrait être affiché, une fois l'octet correspondant identifié.
