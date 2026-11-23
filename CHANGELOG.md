# Historique des versions

Chaque section s'affiche comme note lors de la mise à jour en ligne (Réglages et informations → Rechercher une mise à jour).

## 1.9.1
- Veille hors-gel : au passage en Veille, la consigne est d'abord abaissée à 26 °C (eau maintenue vers 15 °C) ; à la sortie, le mode est rétabli puis la consigne de retour (celle d'avant la veille, 34 °C par défaut). S'applique aux boutons de la page, à la programmation et, en option, aux changements faits au clavier du spa. Réglable dans Programmation → Veille hors-gel.

## 1.9.0
- Horloge du spa : mise à l'heure depuis la page (touches Time, Mode/Prog, Warm, Cool, avec vérification à chaque appui) et mise à l'heure automatique quand l'écart atteint 2 min, changements d'heure été/hiver compris. L'automatique démarre après une première mise à l'heure réussie.
- Relevé des cycles de filtration (lecture seule, Time puis Mode/Prog) : affichage et code de menu notés à chaque étape, pour préparer leur réglage depuis la page.
- Correction : l'accusé de réception des touches est lu dans toutes les trames d'état, y compris quand l'écran n'affiche pas une température (menus heure et filtration). Avant, ces appuis étaient considérés comme perdus.

## 1.8.2
- Numérotation officielle Molex dans la documentation (circuit 1 = triangle imprimé sur la carte) ; les messages parlent du « signal de sélection (circuit 2) » au lieu de la « broche 5 » du projet d'origine.

## 1.8.1
- Programmation : changements de mode de chauffe chaque semaine (ex. Standard le vendredi à 18:00, Économie le dimanche à 20:00) ou à une date précise. Prochain changement affiché sous le mode.
- Codes des touches modifiables depuis Réglages (coller un code capturé, bouton Tester), sans recompiler.
- Tuile Pompe 3, affichée dès que le code de la touche Jets 3 est renseigné.
- Heure du module en heure de France (passage été/hiver automatique) et comparaison avec l'heure du spa.
- Mémoire flash réorganisée (1,9 Mo pour le programme au lieu de 1,25 Mo) : nécessite une installation complète par USB.

## 1.8.0
- Nouvel espion du bus (section « Espion du bus » de la page) : mode simple (seulement les trames qui changent) et mode avancé (toutes les trames).
- Le module garde les dernières trames en mémoire vive (tampon tournant, rien n'est écrit en mémoire flash) ; la page accumule tout l'historique sur le téléphone.
- Bouton « Enregistrer sur le téléphone » (fichier texte à envoyer pour analyse), repères horodatés, filtre par type de trame, octets modifiés surlignés.

## 1.7.0
- Version française : interface, journal, messages d'erreur et portail Wi-Fi traduits.
- Tuiles renommées : Pompe 1, Pompe 2, Blower, Éclairage.
- Les mises à jour en ligne viennent désormais du dépôt fquillerier/jacuzzi-balboa.

## Versions précédentes

Les versions 1.6.1 et antérieures sont celles du projet d'origine (en roumain) : voir [zaiclopz/jacuzzi-balboa](https://github.com/zaiclopz/jacuzzi-balboa/blob/main/CHANGELOG.md).
