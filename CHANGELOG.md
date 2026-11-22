# Historique des versions

Chaque section s'affiche comme note lors de la mise à jour en ligne (Réglages et informations → Rechercher une mise à jour).

## 1.7.0
- Version française : interface, journal, messages d'erreur et portail Wi-Fi traduits.
- Tuiles renommées : Pompe 1, Pompe 2, Blower, Éclairage.
- Les mises à jour en ligne viennent désormais du dépôt fquillerier/jacuzzi-balboa.

## Versions précédentes (projet d'origine, en roumain)

### 1.6.1
- Reparat: modul curent (Standard / Economic / Somn) e cunoscut și fără Home Assistant, deci schimbarea modului merge din prima și modul activ apare selectat în pagină.
- Reparat: o comandă pierdută nu mai apare ca acceptată. Se consideră acceptată doar când jacuzzi-ul răspunde cu codul ei.

### 1.6.0
- Actualizare online: butonul „Verifică actualizări” arată dacă există o versiune nouă, iar „Instalează” o descarcă și o instalează direct de pe GitHub. Fișierul e verificat înainte de instalare.
- Reparat: modulul nu mai pierde comenzile după repararea buclei de reporniri (golirea bufferului se face din nou exact la impulsul de pe pinul 5).

### 1.5.1
- Reparată bucla de reporniri cu pinul 5 conectat (watchdog „întreruperi blocate”).
- Jurnalul arată câte impulsuri pe secundă vin pe pinul 5. Fără linii duplicate în jurnal.

### 1.5.0
- Jurnalul rămâne după repornire, iar ultimele linii rămân și după o pană de curent. Liniile au data și ora.

### 1.4.1
- Buton „Exportă fișier” în jurnal. Fără pinul 5 conectat, comenzile sunt oprite.

### 1.4.0
- Jurnal de depanare, captarea butoanelor panoului fizic, watchdog pentru pagină și WiFi, expirarea comenzilor fără răspuns.
