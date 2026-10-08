# ESPHome – Truma Aventa 2e génération (Bluetooth)

Pilotez une climatisation de toit **Truma Aventa 2e génération** depuis Home Assistant, sans panneau
iNet X : un ESP32 sous ESPHome, qui peut déjà servir de proxy Bluetooth, se connecte directement à la
clim en Bluetooth Low Energy et parle le même protocole que l'app **Truma iNet X**.

> **Statut : expérimental.** Le protocole (iNet X, « TruMessageV3 » + CBOR) a été validé sur du
> matériel réel par le projet [daaaaan/truma-inetx-ble](https://github.com/daaaaan/truma-inetx-ble)
> (panneau iNet X + chauffage Combi). L'Aventa 2 utilise l'app iNet X, donc a priori le même
> protocole. Le comportement exact de l'Aventa **sans panneau** reste à confirmer : appairage,
> adresses internes, valeurs des modes. Le composant est conçu pour le découvrir et le journaliser
> tout seul. Projet non affilié à Truma.

## Sommaire

- [Ce que vous obtenez](#ce-que-vous-obtenez)
- [Prérequis](#prérequis)
- [Installation depuis GitHub](#installation-depuis-github)
  - [Option A – composant externe (recommandé)](#option-a--composant-externe-recommandé)
  - [Option B – package clé en main](#option-b--package-clé-en-main)
- [Premier appairage et adresses tournantes (RPA)](#premier-appairage-et-adresses-tournantes-rpa)
- [Étalonnage avec la télécommande d'origine](#étalonnage-avec-la-télécommande-dorigine)
- [Synchronisation avec la télécommande](#synchronisation-avec-la-télécommande)
- [Explorer d'autres paramètres](#explorer-dautres-paramètres)
- [Référence de configuration](#référence-de-configuration)
- [Dépannage](#dépannage)
- [Développement](#développement)

## Ce que vous obtenez

| Entité Home Assistant | Rôle |
|---|---|
| `climate` **Aventa** | marche/arrêt, mode (froid, chauffage, ventilation, déshumidification, auto), consigne, température, vitesse de ventilation (Auto, Low, Medium, High, Quiet/Nuit), action en cours. Consigne et ventilation suivent la télécommande |
| `light` éclairage | marche/arrêt et luminosité de l'éclairage de la clim, synchronisés avec la télécommande |
| `sensor` température intérieure | température mesurée par la clim |
| `binary_sensor` connectée | session iNet X opérationnelle |
| `text_sensor` état Bluetooth | `disconnected`, `connecting`, `securing`, `registering`, `ready`… |
| `switch` connexion Bluetooth | coupe/rétablit la connexion de l'ESP32 (pour libérer la clim) |
| `button` appairer | trouve l'Aventa la plus proche, s'y connecte et fait le bonding |
| `button` oublier l'appairage | supprime le bond et l'adresse mémorisée |
| `button` relire / journaliser les paramètres | diagnostic |
| action `esphome.<nœud>_truma_write` | écrire n'importe quel paramètre iNet X depuis Home Assistant (package, ou quelques lignes en option A) |

Toutes les correspondances entre Home Assistant et les valeurs iNet X sont configurables en YAML.
Aucune n'est figée dans le code C++.

## Prérequis

- **ESPHome 2026.9 ou plus récent** : testé avec 2026.9.1 et la branche de développement 2026.11.
- Un **ESP32** avec le framework **esp-idf**. ESP32 classique, S3 ou C3.
- Si l'ESP32 est déjà un proxy Bluetooth : chaque connexion active consomme un emplacement. Réglez
  `esp32_ble: max_connections: 4`, soit 3 pour le proxy et 1 pour l'Aventa. Le package le fait
  avec sa variable `max_connections`.

## Installation depuis GitHub

Deux façons de l'utiliser, avec le même code :

- **Option A – composant externe** : vous référencez seulement le composant `truma_inetx` et vous
  déclarez vous-même les entités voulues dans votre YAML. Tout est visible et modifiable chez vous.
- **Option B – package** : une seule ligne `packages:` ajoute un ensemble complet d'entités, réglé
  par quelques variables.

### Option A – composant externe (recommandé)

```yaml
esp32_ble:
  max_connections: 4    # 3 pour le proxy Bluetooth + 1 pour l'Aventa

external_components:
  - source: github://Phil-ibert/Truma-Aventa2-ESPHome@main
    components: [truma_inetx]
    refresh: 1d

ble_client:
  - id: aventa_ble
    mac_address: "00:00:00:00:00:00"   # voir « Premier appairage »

truma_inetx:
  - id: aventa
    ble_client_id: aventa_ble
    # pin: 123456          # si l'Aventa demande un code

climate:
  - platform: truma_inetx
    truma_inetx_id: aventa
    name: "Aventa"         # modes, consigne, ventilation, température : valeurs Aventa par défaut

light:
  - platform: truma_inetx
    truma_inetx_id: aventa
    name: "Aventa éclairage"

button:
  - platform: truma_inetx
    truma_inetx_id: aventa
    type: pair             # aussi : forget_pairing, refresh, dump_parameters
    name: "Aventa appairer"
```

L'exemple complet, avec état de connexion, horloge et bouton « oublier
l'appairage », est dans [examples/aventa-component.yaml](examples/aventa-component.yaml).

### Option B – package clé en main

Ajoutez ce bloc à la configuration YAML de votre ESP32 existant :

```yaml
packages:
  truma_aventa:
    url: https://github.com/Phil-ibert/Truma-Aventa2-ESPHome
    ref: main            # ou une version figée (tag), ex. v0.1.0
    refresh: 1d
    files:
      - path: packages/truma-aventa.yaml
        vars:
          aventa_mac: "00:00:00:00:00:00"   # voir « Premier appairage »
          aventa_pin: ""                    # code à 6 chiffres si l'Aventa en demande un
          aventa_name: "Aventa"
          truma_source: "github://Phil-ibert/Truma-Aventa2-ESPHome@main"
```

Installez le firmware. Les mises à jour du composant arrivent ensuite à chaque recompilation
(`refresh: 1d`). Pour figer une version, utilisez un tag dans `ref` et `truma_source`.

Un exemple complet est disponible : [examples/aventa-proxy.yaml](examples/aventa-proxy.yaml).

Dans les deux options, `@main` suit la dernière version. Remplacez-le par un tag (ex. `@v0.1.0`)
pour figer une version.

## Premier appairage et adresses tournantes (RPA)

Un appareil Bluetooth peut publier une **adresse privée résolvable (RPA)**. Cette adresse change
périodiquement (souvent toutes les 15 minutes) et à chaque redémarrage. Une adresse MAC fixe dans le
YAML ne suffirait alors pas. Le composant gère ce cas.

**Ce qui se passe après l'appairage avec bonding.** L'Aventa transmet sa clé d'identité (IRK). La
pile Bluetooth de l'ESP32 (Bluedroid) la stocke en flash avec le bond. Ensuite, elle **résout
elle-même** chaque nouvelle RPA :
- elle présente à ESPHome une adresse stable, celle enregistrée avec le bond ;
- elle se connecte avec la dernière RPA vue.

Ce fonctionnement est vérifié dans le code source d'ESP-IDF 5.5 : `btm_ble_gap.c` et `l2c_ble.c`.
La liaison survit donc :
- **à une coupure de courant de la clim** : nouvelle RPA, mais même IRK ;
- **à un redémarrage de l'ESP32** : le bond et l'IRK sont en NVS. Le composant mémorise en plus
  l'adresse stable et la réapplique au démarrage.

**Procédure du premier appairage :**

1. Installez le firmware avec `mac_address: "00:00:00:00:00:00"` (option A) ou
   `aventa_mac: "00:00:00:00:00:00"` (option B).
2. Ouvrez les logs. Chaque appareil Truma entendu y apparaît une fois, avec le type de son adresse :
   ```
   [truma_inetx] Truma device heard: 6A:1F:..., resolvable private (RPA, rotates), RSSI -58 dBm, name '...'
   ```
   - `public (fixed)` ou `random static` : l'adresse est fixe. Vous pouvez la mettre directement
     dans `mac_address` / `aventa_mac`.
   - `resolvable private (RPA, rotates)` : il faut passer par le bonding, à l'étape suivante.
3. Si besoin, mettez la clim en mode appairage Bluetooth (voir sa notice). Appuyez ensuite sur
   **« Aventa appairer »** dans Home Assistant. L'ESP32 écoute 8 secondes, choisit l'appareil Truma
   le plus proche (meilleur signal), s'y connecte et fait le bonding. Si l'Aventa demande un code,
   renseignez `pin` et `esp32_ble: io_capability: keyboard_only` (option B : variables
   `aventa_pin` et `ble_io_capability: "keyboard_only"`).
4. Les logs confirment le résultat :
   ```
   [truma_inetx] Bond stored: address XX:..., identity address YY:..., IRK received: ...
   [truma_inetx] Address XX:... remembered for the next restarts
   ```
   Vous pouvez reporter cette adresse dans `mac_address` / `aventa_mac`. Ce n'est pas obligatoire.

> **À éviter :** ré-appairer en boucle. La clim mémorise un nombre limité d'appareils. Un excès de
> bonds pourrait évincer la télécommande d'origine.

**Cas particulier.** Si la clim change d'adresse **sans** proposer de bonding, ajoutez
`device_name: "<nom publié>"` (visible dans les logs) au composant `truma_inetx`. L'ESP32 suivra
alors la clim par son nom.

## Étalonnage avec la télécommande d'origine

Les valeurs par défaut ont été relevées sur une Aventa compact 2e génération (logiciel 1.6).
Pour un autre modèle ou une autre version, la télécommande d'origine sert de référence.

**Comment l'Aventa s'organise.** Elle contient deux équipements iNet X :
- `0x0101` « iNet X Interface AC » : le mode (`RoomClimate.Mode`) et la consigne du mode auto
  (`RoomClimate.TgtTemp`) ;
- `0x0801` le climatiseur : consigne et vitesse de ventilation **du mode actif**,
  `AirCooling.TgtTemp` / `AirCooling.Mode` en froid, `AirHeating.TgtTemp` / `AirHeating.Mode` en
  chauffage, `AirCirculation.FanLevel` en ventilation. C'est là qu'écrit la télécommande.

L'entité climatisation lit et écrit donc les paramètres du mode en cours, comme la télécommande.

1. Une fois connecté, les logs listent chaque paramètre découvert :
   ```
   [truma_inetx] New parameter RoomClimate.Mode = 2 (from 0x0101)
   [truma_inetx] [0x0801] AirCooling.Mode = 0 enum=[{"n":"Auto","v":0},{"n":"Low","v":1},...]
   ```
2. Changez le mode, la consigne et la ventilation **avec la télécommande**. Chaque changement
   s'affiche, par exemple `RoomClimate.Mode: 0 -> 2` ou `AirCooling.TgtTemp: 220 -> 230`. Une
   valeur sans correspondance est signalée une fois
   (`... has no fan mode mapping: add it in the climate configuration`).
3. Si une valeur diffère, corrigez-la dans votre YAML. Avec l'option A, directement dans votre
   entité `climate`. Avec l'option B, sans toucher au package :
   ```yaml
   climate:
     - id: !extend truma_climate
       mode_parameter:
         topic: RoomClimate
         parameter: Mode
         values:
           "OFF": 0
           COOL: 2
           HEAT: 3
           FAN_ONLY: 5
           DRY: 6
   ```
4. Le bouton **« journaliser les paramètres »** affiche à tout moment tous les paramètres connus.

Pour voir le détail de chaque trame échangée, utilisez la variable `log_frames: "true"`.

## Synchronisation avec la télécommande

La télécommande de l'Aventa 2 est **Bluetooth**, pas infrarouge. C'est donc la clim qui détient
l'état de référence :
- **Télécommande → Home Assistant** : la clim diffuse chaque changement à ses abonnés, dont l'ESP32.
- **Home Assistant → télécommande** : l'ESP32 modifie l'état de la clim elle-même. L'affichage de la
  télécommande suit si elle est abonnée, ce qui est probable.

Point à vérifier sur votre installation : le nombre de connexions simultanées acceptées par l'Aventa
(télécommande + app + ESP32). Si l'app ne parvient plus à se connecter, coupez temporairement
l'interrupteur **« connexion Bluetooth »**.

## Explorer d'autres paramètres

- **Lire n'importe quel paramètre** avec les plateformes génériques `sensor`, `binary_sensor`,
  `text_sensor`, `number`, `select` ou `switch` du composant `truma_inetx` :
  ```yaml
  select:
    - platform: truma_inetx
      truma_inetx_id: truma
      name: "Aventa éclairage"
      topic: AmbientLight
      parameter: Active
      options:
        0: "Éteint"
        1: "Allumé"
  ```
- **Écrire un paramètre depuis Home Assistant** : allez dans Outils de développement > Actions,
  choisissez `ESPHome: <nœud>_truma_write`, puis renseignez `topic`, `parameter` et `value`. Cette
  action est fournie par le package. Avec l'option A, ajoutez-la vous-même :
  ```yaml
  api:
    actions:
      - action: truma_write
        variables: {topic: string, parameter: string, value: int}
        then:
          - lambda: id(aventa).write_int(topic, parameter, value);
  ```
- **Depuis une lambda** : `id(truma).write_int("AirCooling", "Mode", 1);`, `id(truma).refresh();`,
  `id(truma).get_value("RoomClimate", "TgtTemp")`.

Le protocole et la liste des topics connus sont décrits dans [docs/protocol.md](docs/protocol.md).
La procédure de capture avec un sniffer nRF et Wireshark est dans
[docs/capture-ble.md](docs/capture-ble.md).

## Référence de configuration

### `truma_inetx` (composant principal)

| Option | Défaut | Description |
|---|---|---|
| `ble_client_id` | — | le `ble_client` de l'Aventa |
| `time_id` | — | horloge à envoyer à la clim (`SystemTime`), facultative |
| `pin` | aucun | code à 6 chiffres envoyé si la clim en demande un (avec `esp32_ble: io_capability: keyboard_only`) |
| `encryption` | `true` | lance l'appairage/chiffrement à la connexion |
| `user_name` | `ESPHome` | nom présenté à la clim (comme un téléphone) |
| `muid` / `uuid` | dérivés du nom du nœud | identité stable exigée par la clim. Ne la changez pas après l'appairage |
| `topics` | les 33 topics de l'app | topics auxquels s'abonner |
| `discovery_addresses` | `0x0101, 0x0801` | équipements interrogés au démarrage. Les adresses vues ensuite sont ajoutées automatiquement |
| `default_destination` | `0x0101` | destination des écritures tant qu'aucune adresse n'est apprise |
| `destinations` | — | forcer une destination par topic, ex. `AirCooling: 0x0801` |
| `optimistic` | `true` | met à jour Home Assistant dès l'envoi, sans attendre la confirmation de la clim |
| `remember_address` | `true` | mémorise l'adresse stable après appairage (voir RPA) |
| `device_name` | — | suit la clim par son nom publié si elle change d'adresse sans bonding |
| `log_advertisements` | `true` | journalise les appareils Truma entendus |
| `log_frames` | `false` | journalise chaque trame décodée |
| `frame_delay` | `100ms` | délai minimal entre deux messages |
| `tx_power` | non modifiée (+3 dBm, valeur d'ESP-IDF) | puissance d'émission Bluetooth de l'ESP32 en dBm : -12, -9, -6, -3, 0, 3, 6 ou 9 (maximum). À augmenter si la clim ne répond pas aux demandes de connexion (`reason 0x3e`). S'applique à toute la radio Bluetooth de l'ESP32 (proxy compris) |
| `poll_interval` | `60s` | relit périodiquement tous les paramètres (filet de sécurité si la clim ne signale pas d'elle-même un changement fait à la télécommande). `never` pour désactiver |

### Plateformes d'entités

Toutes acceptent `truma_inetx_id` et les options standard d'ESPHome. Sauf `climate`, `light` et
`button`, elles prennent aussi `topic` et `parameter`.

- `climate` : sans autre option, utilise les valeurs Aventa par défaut :

  | Option | Défaut |
  |---|---|
  | `mode_parameter` | `RoomClimate.Mode` : `OFF` 0, `AUTO` 1, `COOL` 2, `HEAT` 4, `FAN_ONLY` 5, `DRY` 6 |
  | `target_temperature_parameter` | `RoomClimate.TgtTemp` ; en `COOL` : `AirCooling.TgtTemp`, en `HEAT` : `AirHeating.TgtTemp` |
  | `current_temperature_parameter` | `AirCooling.Temp` |
  | `fan_mode_parameter` | `AirCooling.Mode` : `AUTO` 0, `LOW` 1, `MEDIUM` 2, `HIGH` 3, `QUIET` 4 (Nuit) ; en `HEAT` : `AirHeating.Mode` ; en `FAN_ONLY` : `AirCirculation.FanLevel` (`LOW` 1, `MEDIUM` 2, `HIGH` 3) |
  | `action_parameter` | `AirCooling.Active` (refroidit), `AirHeating.Active` (chauffe), `AirDehumid.Active` (déshumidifie) |
  | `preset_parameter` | aucun |
  | `temperature_multiplier` | 0.1 (l'iNet X compte en dixièmes de degré) |

  Chaque `*_parameter` prend `topic` et `parameter`, et `values` (valeur Home Assistant → valeur
  iNet X) quand il y a une correspondance. `per_mode` remplace le paramètre (et éventuellement
  les valeurs) pour un mode donné :
  ```yaml
  fan_mode_parameter:
    topic: AirCooling
    parameter: Mode
    values: {AUTO: 0, LOW: 1, MEDIUM: 2, HIGH: 3, "Nuit": 4}
    per_mode:
      HEAT: {topic: AirHeating, parameter: Mode}       # mêmes valeurs
      FAN_ONLY:
        topic: AirCirculation
        parameter: FanLevel
        values: {LOW: 1, MEDIUM: 2, HIGH: 3}
  ```
  Les noms de vitesse standard (`AUTO`, `LOW`, `MEDIUM`, `HIGH`, `QUIET`…) sont traduits par
  Home Assistant ; tout autre nom (ex. `"Nuit"`) est affiché tel quel. `custom_fan_modes: false`
  masque ces noms personnalisés. La valeur `false` désactive un paramètre optionnel
  (`fan_mode_parameter: false` retire la ventilation de l'entité). `action_parameter` accepte une
  source ou une liste : la première qui indique une action en cours l'emporte.
- `light` : sans autre option, pilote l'éclairage de l'Aventa :

  | Option | Défaut |
  |---|---|
  | `active_parameter` | `AmbientLight.Active` avec `on_value` 1 et `off_value` 0 |
  | `brightness_parameter` | `AmbientLight.LightStep`, `max_value` 100 (= 100 %), `min_value` 1. `false` pour une lumière marche/arrêt sans luminosité |

  La luminosité est appliquée directement (`gamma_correct: 1`, pas de transition par défaut).
  Si la clim n'accepte que certains niveaux, elle renvoie le niveau retenu et Home Assistant
  l'affiche. Un allumage à la télécommande (`Active` puis `LightStep`) est repris tel quel, sans
  renvoyer l'ancienne luminosité de Home Assistant à la clim.
- `button` : `type` parmi `pair` (premier appairage), `forget_pairing` (supprime le bond et
  l'adresse mémorisée), `refresh` (relit tous les paramètres), `dump_parameters` (les journalise).
- `sensor` / `number` : `multiplier`. `number` prend aussi `min_value`, `max_value` et `step`.
- `select` : `options` (valeur iNet X → libellé).
- `switch` : `on_value` (1) et `off_value` (0).
- `binary_sensor` / `text_sensor` : sans `topic`, ils donnent l'état de la connexion.

## Dépannage

| Symptôme | Piste |
|---|---|
| Aucune ligne `Truma device heard` | La clim est-elle alimentée ? Son Bluetooth est-il activé ? L'ESP32 est-il à portée ? |
| Avertissement `BLE components require N connection slot(s)` | Augmentez `max_connections` ou réduisez `bluetooth_proxy: connection_slots`. |
| `Pairing failed (reason 0x..)` | PIN erroné (`aventa_pin`), ou clim pas en mode appairage. Après une réinitialisation de la clim, utilisez « oublier l'appairage » puis « appairer ». |
| `Write refused by the device: pairing required` | Appairage nécessaire : vérifiez `encryption` et `pin`. |
| `Truma iNet X characteristics not found` | Le composant cherche `FC314001`–`FC314003` dans tous les services. Si ce message apparaît, la disposition GATT (imprimée juste en dessous) est inattendue : ouvrez une issue avec ce log. |
| `No registration response` | Protocole différent sur l'Aventa : activez `log_frames` et faites une capture (voir [docs/capture-ble.md](docs/capture-ble.md)). |
| Un mode ne s'applique pas | Étalonnez les valeurs avec la télécommande (voir plus haut). |
| `Message from 0x.... ignored: decoding it needs about N bytes of memory` | Un message reçu demanderait plus de mémoire que l'ESP32 n'en a de libre : il est ignoré plutôt que de faire planter le firmware. Si cela se répète, envoyez ce log (et, si possible, la même séquence avec `log_frames: true`). |
| Reconnexion lente : `Connection open error, status=133` avec `reason 0x3e`, puis `N connection attempts ... failed` | La clim n'a pas répondu à la demande de connexion. C'est un problème radio, pas un problème d'appairage : l'ESP32 réessaie tout seul et finit par se connecter (`Connected ... after N failed attempts`). Comparez le RSSI affiché à celui de l'appairage. Rapprochez l'ESP32 de la clim ou dégagez son antenne, ou augmentez `tx_power` (6, voire 9). Un Wi-Fi faible occupe aussi davantage la radio que l'ESP32 partage avec le Bluetooth. |

## Développement

```
components/truma_inetx/   composant ESPHome externe
  cbor.*  frame.*          codec CBOR + trames TruMessageV3 (C++ pur, testable sur PC)
  truma_inetx.*            connexion BLE, transport, session iNet X, adresses/bonding
  climate/ button/ sensor/ ... plateformes d'entités
packages/truma-aventa.yaml package prêt à l'emploi (option B)
examples/                  exemples option A (composant) et option B (package)
tests/                     tests unitaires du codec + configuration de compilation CI
docs/                      protocole et guide de capture BLE
```

- `sh tests/run_tests.sh` : tests du codec avec ASan/UBSan et fuzzing. Les trames construites sont
  identiques octet pour octet à celles de l'implémentation de référence.
- La CI GitHub (`.github/workflows/ci.yml`) compile `tests/test-esp32.yaml` avec les versions
  stable et bêta d'ESPHome, à chaque push et chaque semaine. Une nouvelle version d'ESPHome qui
  casserait le composant est ainsi détectée tôt.

## Crédits et licence

- Protocole iNet X documenté par [daaaaan/truma-inetx-ble](https://github.com/daaaaan/truma-inetx-ble)
  (analyse de trafic et tests d'interopérabilité). Ce dépôt est une implémentation indépendante pour
  ESPHome. Aucun code n'en est repris.
- Licence MIT. Truma, Aventa et iNet X sont des marques de Truma Gerätetechnik GmbH & Co. KG.
  Projet non affilié à Truma, à utiliser à vos risques.
