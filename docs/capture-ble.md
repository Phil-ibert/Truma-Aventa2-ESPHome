# Capturer le Bluetooth de l'Aventa (nRF Sniffer + Wireshark)

Le composant journalise déjà tout ce qu'il échange avec la clim (`log_frames: "true"`). Il est en
effet l'une des deux extrémités de la liaison, donc il voit les messages **en clair**, même quand
le lien radio est chiffré.

Le sniffer sert à observer ce que l'ESP32 ne voit pas :
- la radio elle-même : publicités, type d'adresse, méthode d'appairage ;
- les échanges **entre la clim et d'autres appareils** : la télécommande et l'app iNet X.

## Matériel et logiciel

- Une clé ou carte Nordic avec le firmware **nRF Sniffer for Bluetooth LE** (nRF52840 Dongle ou
  DK, nRF52832 DK…).
- **Wireshark** avec le plugin extcap nRF Sniffer, installé selon la documentation Nordic. Dans
  Wireshark, l'interface apparaît sous le nom « nRF Sniffer for Bluetooth LE ».
- Filtres d'affichage utiles :
  - `btle.advertising_header` : publicités ;
  - `btsmp` : appairage ;
  - `btatt` : échanges GATT ;
  - `btle.advertising_address == aa:bb:cc:dd:ee:ff` : un seul appareil.

## Capture 1 – Publicités et type d'adresse (10 min + coupure de courant)

But : savoir si l'Aventa utilise une adresse tournante (RPA).

1. Lancez la capture sur les trois canaux de publicité (réglage par défaut du sniffer).
2. Repérez l'Aventa : identifiant constructeur `0x0C73`, ou UUID de service `FC31…-F3B2-11E8-…`.
3. Relevez le champ `TxAdd` de l'en-tête :
   - `TxAdd = 0` : adresse publique, fixe.
   - `TxAdd = 1` : adresse aléatoire. Les deux bits de poids fort donnent son type : `11` statique,
     `01` résolvable (RPA), `00` non résolvable.
4. Laissez tourner **au moins 20 minutes**, puis **coupez et rallumez la clim**. Notez si
   l'adresse change.
5. Notez aussi le nom publié et les données constructeur. S'ils sont uniques à votre clim, l'option
   `device_name` peut servir à la suivre.

## Capture 2 – Télécommande d'origine ↔ Aventa

But : comprendre la cohabitation des connexions et la synchronisation.

1. Filtrez sur l'adresse de la clim et appuyez sur des touches de la télécommande.
2. Observez :
   - si la télécommande **reste connectée** en permanence ou se connecte à chaque appui
     (`CONNECT_IND` dans la capture) ;
   - si la clim **continue à publier** pendant qu'elle est connectée. Si oui, elle accepte
     probablement plusieurs connexions simultanées.
3. Si le lien n'est pas chiffré, les trames `btatt` vers `FC314002` / depuis `FC314003` montrent
   le protocole de la télécommande. Les charges commencent par l'en-tête TruMessageV3 décrit dans
   [protocol.md](protocol.md).

## Capture 3 – Appairage de l'app iNet X

But : connaître la méthode d'appairage, et savoir si le trafic de l'app peut être déchiffré.

1. Dans le sniffer, choisissez de **suivre** l'Aventa (liste « Device » de la barre d'outils). Le
   sniffer pourra alors suivre la connexion.
2. Supprimez l'Aventa de l'app et du téléphone, lancez la capture, puis appairez de nouveau.
3. Dans les paquets `btsmp` (Pairing Request / Pairing Response), relevez :
   - **IO Capability** et le drapeau **MITM** : saisie d'un code ou « Just Works » ;
   - le drapeau **SC** (LE Secure Connections) ;
   - les champs **Initiator/Responder Key Distribution**, en particulier le bit **IdKey**. S'il est
     présent, l'Aventa transmet une IRK, ce qui confirme l'usage possible d'adresses RPA.
4. Selon le cas :
   - **Appairage « Legacy »** (SC absent) : Wireshark peut déchiffrer la suite si le code (ou 0
     pour Just Works) est renseigné dans les préférences du sniffer.
   - **LE Secure Connections** : le trafic chiffré n'est pas déchiffrable par écoute passive. Ce
     n'est pas bloquant. Activez `log_frames` sur l'ESP32 : il affiche le même dialogue en clair,
     de son côté.

## Ce qu'il est utile de remonter

Une capture `.pcapng` n'est pas indispensable. Ces informations suffisent à ajuster le composant :
- le type d'adresse, et s'il change à la coupure de courant ;
- la méthode d'appairage (IO capability, MITM, SC, présence d'IdKey) ;
- l'arborescence GATT : avec `log_frames: "true"`, l'ESP32 l'imprime à la connexion ;
- les logs ESPHome d'une connexion complète, avec `log_frames: "true"`, pendant que vous changez
  le mode et la consigne **à la télécommande**.

> Les captures contiennent l'adresse de votre clim et éventuellement des clés d'appairage. Ne les
> publiez pas telles quelles.
