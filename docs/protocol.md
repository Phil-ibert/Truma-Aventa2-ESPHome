# Protocole Truma iNet X en Bluetooth LE (tel qu'implémenté ici)

Cette page résume ce que fait le composant. Les détails proviennent de l'analyse publiée par
[daaaaan/truma-inetx-ble](https://github.com/daaaaan/truma-inetx-ble), validée sur un panneau
iNet X avec un chauffage Combi D 4 E. Ce qui n'est **pas encore confirmé sur une Aventa 2 sans
panneau** est signalé par ⚠️.

## 1. GATT

Tous les UUID partagent le suffixe `-F3B2-11E8-8EB2-F2801F1B9FD1`.

| Élément | UUID | Usage |
|---|---|---|
| Service | `FC314000-…` | **Aventa 2e génération** (observé) |
| Service | `F47BBBAC-…` | panneau iNet X |
| CMD | `FC314001-…` | écriture avec réponse + notifications : contrôle du transport |
| DATA_W | `FC314002-…` | écriture sans réponse : messages vers l'appareil |
| DATA_R | `FC314003-…` | notifications : messages de l'appareil |
| CMD_ALT | `FC314004-…` | **ne pas s'y abonner** (casse le transport) |

Publicités : identifiant constructeur Truma `0x0C73`, UUID de service `FC31xxxx-…`.

**Observé sur une Aventa 2e génération :**
- adresse **privée résolvable (RPA)** : les deux bits de poids fort valent `01`, par ex. `51:11:…` ;
- le service GAP contient « Central Address Resolution » (`0x2AA6`) et « Resolvable Private
  Address Only » (`0x2AC9`) ; le bonding est donc nécessaire pour se reconnecter après une rotation ;
- service Device Information (`0x180A`) présent ;
- propriétés : CMD `0x18` (écriture avec réponse + notification), DATA_W `0x04` (écriture sans
  réponse), DATA_R et CMD_ALT `0x10` (notification) ;
- MTU négociée : 251.

## 2. Transport (caractéristique CMD)

Envoi d'un message de N octets :

```
ESP32 -> CMD    01 <N lo> <N hi>     annonce de la taille
ESP32 <- CMD    81 00                prêt
ESP32 -> DATA_W <message>            (découpé en morceaux de MTU-3 si nécessaire)
ESP32 <- CMD    f0 01                données reçues (01 = transfert terminé)
ESP32 <- CMD    83 xx 00             accusé du message (asynchrone)
ESP32 -> CMD    03 00                confirmation
```

Réception : chaque message reçu sur DATA_R est acquitté par `f0 01` sur CMD.
⚠️ Le comportement avec une MTU de 23 n'a jamais été observé : la MTU négociée est normalement de 517.

## 3. Trame TruMessageV3

```
0-1   destination (LE)        6     type de contrôle
2-3   source (LE)             7-15  en-tête de segmentation (zéros si non segmenté)
4-5   taille = charge + 9     16    sous-type (ex. type MBP)
                              17    identifiant de corrélation
                              18…   charge CBOR
```

Types de contrôle : `01` enregistrement, `02` découverte d'équipements, `03` Message Broker Protocol
(MBP), `04` fichiers, `05` sécurité, `06` mise à jour firmware.

Sous-types MBP : `00` INFO (appareil → ESP32), `01` WRITE, `02` SUBSCRIBE, `04` découverte de
paramètres, `82`/`84` réponses correspondantes.

## 4. Session

1. **Enregistrement** : `{"pv": [5, 1]}` vers `0xFFFF`, depuis `0x0500`. La réponse
   `{"addr": n}` donne l'adresse à utiliser comme source ensuite.
2. **Abonnement** aux topics, 10 par message : `{"tn": ["AirCooling", …]}` vers `0x0000`.
3. **Identité** : écritures `MobileIdentity.UserName/Muid/Uuid`, précédées de `SystemTime.Time`,
   puis `{"LastMessage": 1}`. L'appareil reconnaît ses clients à leur Muid/Uuid : l'identité doit
   rester stable. Le composant la dérive du nom du nœud ESPHome.
4. **Découverte de paramètres** (charge vide) vers chaque équipement. La réponse
   `{"topics": [{"tn", "parameters": [{"pn", "v", "min", "max", "enum", …}]}]}` donne les valeurs
   courantes et les plages.
5. **Fonctionnement** : messages INFO `{"tn", "pn", "v"}` à chaque changement, écritures
   `{"tn", "pn", "v", "id": 0}`.

**Adresses internes.** Sur un panneau iNet X : `0x0101` (panneau), `0x0201` (chauffage sur le bus
TIN), `0x0202` (Aventa sur le bus TIN).
⚠️ Sur une Aventa seule, les adresses sont inconnues. Le composant envoie chaque écriture à
l'adresse qui publie le topic concerné. Il apprend ces adresses dans les messages INFO et interroge
automatiquement tout nouvel équipement.

## 5. Topics utiles pour la climatisation

Les températures sont en **dixièmes de °C**.

| Topic.Paramètre | Valeurs connues |
|---|---|
| `RoomClimate.Mode` | 0 arrêt, 1 ACC (auto), 2 froid, 3 chauffage, 4 chauffage clim, 5 ventilation, 6 déshumidification |
| `RoomClimate.TgtTemp` | consigne, 160–300 |
| `AirCooling.Active` | 0 arrêt, 1 actif, 2 en veille de régulation |
| `AirCooling.TgtTemp` / `AirCooling.Temp` | consigne / température mesurée |
| `AirCooling.Mode` | 0 confort, 1 rapide |
| `AirCirculation.FanLevel` | niveau de ventilation |
| `AirHeating.*` | chauffage (pompe à chaleur de l'Aventa ou Combi) |

⚠️ Sur l'Aventa, les combinaisons exactes (mode « chauffage » = 3 ou 4 ? ventilation par
`RoomClimate` ou `AirCirculation` ?) se lisent dans les logs en utilisant la télécommande.
