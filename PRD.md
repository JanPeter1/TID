# Product Requirements Document: Texel-Info-Display

**Status:** Umsetzung in Arbeit
**Produkt:** ESP32 CYD-Info-Display für ein Ferienhaus auf Texel
**Sprache der Anzeige:** Deutsch
**Stand:** 3. November 2026

## 1. Produktidee

Ein dauerhaft verfügbares, auf einen ESP32 CYD (Cheap Yellow Display) zugeschnittenes Informationsdisplay für Gäste eines Ferienhauses auf Texel. Die Oberfläche orientiert sich am bereitgestellten Screenshot: dunkles Design, orangefarbene Akzente, kompakte Registerkarten und eine gut lesbare Informationsfläche.

Die Inhalte werden, soweit möglich, regelmäßig aus den angegebenen Webquellen aktualisiert. Das Display soll auch bei vorübergehendem Ausfall von WLAN oder Datenquellen sinnvoll nutzbar bleiben und den letzten erfolgreichen Datenstand anzeigen.

## Umsetzungsstand

- **Bisher am Gerät bestätigt:** ESP-IDF-Projekt für ESP32-2432S028, Displayausrichtung, XPT2046-Touch über separaten SPI-Bus, fünf Icon-Bereiche mit Unterseiten und lokaler Setup-Modus.
- **LVGL-Umstellung implementiert, Hardwareverhalten noch zu prüfen:** LVGL 9.6 mit partiellem RGB565-Framebuffer, SPI-Flush für das ILI9341 und XPT2046 als Pointer-Eingabe. Die UI verwendet scharfe Montserrat-Schriften mit Noto-Sans-Glyphen für `äöüÄÖÜß`.
- **Implementiert, Hardwareverhalten noch zu prüfen:** Wetter-/Marineabruf mit den Seiten „Jetzt“, Fünf-Tage-Vorschau und Gezeitenverlauf, Cache, NTP-Synchronisierung nach WLAN-Verbindung und lokales Setup-Portal ohne Login.
- **Implementiert, Hardwareverhalten noch zu prüfen:** VVV-GraphQL-Abruf, Filterung auf heute/morgen und eine Unterseite je Event (maximal 12 gespeicherte Events); Wetteransichten mit gezeichneten Wetter-Icons.
- **Implementiert, Hardwareverhalten noch zu prüfen:** Belegungskalender lädt öffentliche Tagesbelegungen und Gruppennamen über HTTPS und zeigt sie in einer monatlichen, vertikal scrollbaren Ansicht mit Monatsnavigation.
- **Noch offen:** VVV-Abruf und Cache auf dem Gerät nach Flashen der aktualisierten Partitionstabelle verifizieren, Anzeige verlässlicher Aktualisierungszeitpunkte und OTA-Update-Logik.
- **Implementiert, Hardwareverhalten noch zu prüfen:** Fotoverarbeitung mit EXIF-Datum/GPS-Ortsauflösung und JSON-Manifest; zufälliger HTTPS-Abruf beim Öffnen einer Foto-Unterseite und JPEG-Anzeige über LVGL/TJPGD.
- **Implementiert, Hardwareverhalten noch zu prüfen:** Offline-Infoseiten aus `assets/info/pages.txt` mit Absätzen, Listen und optionalen 80 x 60 RGB565-Bildern. Referenzierte Quelldateien unter `assets/info/source/` werden bei Bedarf beim Build skaliert; ein QR-Code führt zur jeweiligen Quellseite.
- Die aktuelle Refresh-Implementierung verwendet ein Sechs-Stunden-Intervall und startet Abrufe erst nach `IP_EVENT_STA_GOT_IP`; die anfängliche Aktualisierung bleibt während DHCP ausstehend und wird nach erhaltener IP ausgeführt. Große HTTP-Antwortpuffer liegen auf dem Heap. Nach wiederholt gemeldeten Abstürzen im LWIP-TCP/IP-Task wurden dessen Stack auf 8 KB erhöht und Compiler-Stackschutz sowie leichtgewichtige Heap-Prüfung aktiviert. Laufzeitstabilität ist nach erneutem Flashen zu verifizieren; die Firmware protokolliert außerdem den verbleibenden Stack der UI- und Daten-Tasks.

## 2. Ziele

- Gästen schnell nützliche, aktuelle Informationen für ihren Aufenthalt zeigen.
- Die Inhalte auf einem kleinen CYD-Display übersichtlich und mit möglichst wenigen Interaktionen zugänglich machen.
- Termine, Kalender, Fotos, Wetter, Gezeiten und Hausinformationen dynamisch bereitstellen.
- Fehler oder veraltete Daten erkennbar machen, statt Aktualität vorzutäuschen.

### Nicht-Ziele für die erste Version

- Eine allgemeine Texel-Reise-App für Smartphones oder andere Geräte.
- Bearbeiten der Inhalte direkt am Display.
- Ein Backend oder ein öffentlich zugänglicher Dienst, sofern die Quellen direkt und zuverlässig abrufbar sind.
- Eine Garantie, dass beliebige Webseiten ohne stabile Schnittstelle automatisiert ausgelesen werden können.

## 3. Zielgruppe und Nutzungsszenario

**Primäre Nutzer:** Gäste des Ferienhauses, die vor Ort schnell wissen möchten, was heute oder morgen auf Texel stattfindet, wie das Wetter wird und welche Informationen für das Haus wichtig sind.

**Nutzung:** Das Gerät läuft im Haus dauerhaft. Gäste wechseln über die obere Navigation zwischen den Bereichen. Aktuelle Inhalte sind ohne Tastatur oder längere Eingaben lesbar.

## 4. Zielgerät und Bedienkonzept

- Zielplattform: ESP32-2432S028 („CYD“), ESP-IDF 6.1, 320 × 240 Pixel im Querformat, ILI9341 und resistiver XPT2046-Touchcontroller. Ausrichtung und Touchbedienung wurden auf dem Gerät bestätigt.
- Das TFT verwendet SPI2. Der Touchcontroller verwendet gemäß dem funktionierenden Referenzprojekt einen separaten SPI3-Bus: MOSI GPIO 32, MISO GPIO 39, SCLK GPIO 25, CS GPIO 33, IRQ GPIO 36. Die Display-Hintergrundbeleuchtung liegt auf GPIO 21.
- Das Layout soll sich am angehängten Referenzbild orientieren: dunkler Hintergrund, kontrastreiche Texte, orange Hervorhebungen, kompakte Navigationsleiste und Karten-/Listenansichten. Die Anzeige wird mit LVGL 9.6 aufgebaut.
- Hauptbereiche:
  1. **Aktuell**
  2. **Kalender**
  3. **Fotos**
  4. **Wetter**
  5. **Infos**
- Jeder Hauptbereich besitzt mehrere Unterseiten. Abgerundete seitliche `<`- und `>`-Schaltflächen blättern ausschließlich innerhalb des ausgewählten Bereichs; eine Seitenanzeige zeigt die aktuelle Position. Ein Wechsel des Hauptbereichs startet auf dessen erster Unterseite.
- Die Statusleiste zeigt den WLAN-Status und die Signalqualität ausschließlich über farbige Balken: grün ab -60 dBm, gelb ab -75 dBm, darunter beziehungsweise ohne Verbindung rot; ein numerischer RSSI-Wert wird nicht angezeigt. Das WLAN-Symbol steht links von Setup (Zahnrad), Deep Sleep und QR-Code; diese bleiben reine Icon-Schaltflächen.
- Der Deep-Sleep-Modus wird über den Touch-IRQ des XPT2046 beendet. Vor dem Einschlafen wartet das Gerät auf das Loslassen des Touchfelds.
- Die QR-Schaltfläche zeigt einen QR-Code zur Quelle der aktuellen Unterseite, inklusive individueller Event-URL, sofern die Veranstaltungsdaten eine solche enthalten.
- Die Touch-Zonen der seitlichen Navigation erstrecken sich zur einfacheren Bedienung über die gesamte Höhe des Inhaltsbereichs; die sichtbaren Pfeil-Schaltflächen bleiben kompakt und abgerundet.
- Die Statuszeile zeigt die lokale Uhrzeit vor dem Datum in unterschiedlichen Farben. Zeitquelle ist NTP; Zeitzone ist `Europe/Amsterdam` einschließlich Sommerzeit.
- Bedienung ausschließlich über den XPT2046-Touchcontroller; die Touchbedienung wurde am konkreten Gerät bestätigt. Eine spätere Änderung der Koordinatenabbildung muss erneut am Gerät geprüft werden.
- Die Oberfläche muss für die tatsächliche Displayauflösung und Orientierung des ausgewählten Geräts optimiert werden. Der Screenshot dient als visuelle Referenz, nicht als verbindliche Pixelvorlage.
- Lange Inhalte müssen scrollbar oder in kompakte, einzeln navigierbare Karten aufgeteilt sein. Text darf nicht abgeschnitten werden, ohne dass er erreichbar ist.

## 5. Funktionale Anforderungen

### 5.1 Aktuell

- Beim Start wird für höchstens 60 Sekunden ein bildschirmfüllender Begrüßungsbildschirm mit `IMG_20261102_081843` gezeigt; eine Touch-Berührung kann ihn vorzeitig schließen. Wenn nach der NTP-Synchronisierung für den aktuellen Tag eine Belegung vorliegt, erscheint „Willkommen auf Texel, <Name>“ in einer teilweise transparenten Textbox.
- Zeigt aktuelle Veranstaltungen und Termine auf Texel, mindestens für heute und morgen.
- Quelle: [Veranstaltungsübersicht des VVV Texel](https://www.texel.net/de/veranstaltungsuebersicht/).
- Veranstaltungen für das lokale heutige und morgige Datum werden dynamisch über die öffentliche GraphQL-Schnittstelle der VVV-Seite abgerufen. Jeder Treffer erhält eine eigene Unterseite mit Titel, Datum/Uhrzeit und Ort sowie einem QR-Link zur Detailseite; maximal 16 passende Termine werden gespeichert und chronologisch sortiert.
- Zeigt außerdem sehenswerte Orte und Tipps; die Referenzoberfläche sieht Filter/Kategorien wie „Alle“, „Orte & Sights“ und „Kinder & Familie“ sowie eine Karten-/Karussellansicht vor.
- Jeder Eintrag soll, soweit von der Quelle geliefert, Titel, Datum/Uhrzeit, Ort und eine kurze Beschreibung enthalten.
- Wenn Inhalte nicht abrufbar sind, werden gespeicherte letzte Inhalte mit ihrem Aktualisierungszeitpunkt gezeigt. Sind keine gespeicherten Inhalte vorhanden, erscheint eine verständliche Fehlermeldung.

### 5.2 Kalender

- Zeigt den Texelkalender, dynamisch bezogen von `https://familie-riegel.de/tx94/?Texel-Kalender`.
- Der Abruf erfordert laut Nutzerangabe eine Anmeldung. Eine API ist nicht vorhanden; die festgelegte HTML-Struktur wird automatisiert abgerufen und gezielt geparst.
- Kalenderinhalte werden lesbar und passend zum kleinen Display dargestellt, zum Beispiel als Tages-/Terminliste.
- Der geschützte Kalender wird mit den vom Betreiber bereitgestellten Zugangsdaten abgerufen. Zugangsdaten werden nicht in versionierte Dateien oder öffentliche Artefakte geschrieben, sondern bei der lokalen Bereitstellung über eine nicht versionierte Konfiguration beziehungsweise Gerätekonfiguration eingebracht. Im Firmware-Image oder Gerätespeicher hinterlegte Zugangsdaten sind bei physischem Zugriff möglicherweise auslesbar.

### 5.3 Fotos

- `tools/process_photos.py` verarbeitet Bilder aus `assets/photos/source/` und schreibt 256 × 137 baseline-JPEGs bis 12 KiB sowie `assets/photos/images/manifest.json` mit relativen Pfaden, ISO-Datum, Titel und exakter Dateigröße.
- Das Datum stammt aus EXIF `DateTimeOriginal`, ersatzweise aus dem Dateierstellungsdatum. Der Titel wird anhand von EXIF-GPS über Nominatim/OpenStreetMap aufgelöst; ohne GPS beziehungsweise bei einem Lookup-Fehler wird der Dateiname oder die Koordinate verwendet.
- Fehlende und gegenüber der Quelle neuere Bilder werden neu skaliert; unveränderte Ausgaben bleiben erhalten. Das vollständige Verzeichnis `assets/photos/images/` wird manuell per FTP nach `ftp://familie-riegel-de@ftp.familie-riegel.de/tx94/photos/` hochgeladen. FTP-Zugangsdaten werden nicht in versionierte Dateien eingetragen.
- Die Firmware lädt Manifest und jeweils ausgewähltes Foto über HTTPS von `https://familie-riegel.de/tx94/photos/`. Beim Öffnen oder Wechseln einer Foto-Unterseite wird ein zufälliger Eintrag ausgewählt und nur dessen tatsächliche Bytezahl im Heap reserviert. JPEG wird mit LVGL/TJPGD in kleinen Kacheln decodiert, damit kein vollständiger RGB565-Bildpuffer benötigt wird.
- Fehlerhafte oder nicht unterstützte Bilder dürfen die übrige Oberfläche nicht blockieren.
- eventuell hier auch eine einbindung von webcams auf texel (zu finden auf der Webseite des VVV Texel).

### 5.4 Wetter und Gezeiten

- **Standort:** Slufterweg 94, De Cocksdorp, Texel. Für die Abfrage ermittelte OpenStreetMap/Nominatim die Koordinaten **53.1242788° N, 4.8128299° E**. Die Adresse muss nicht auf dem Display erscheinen.
- **Empfohlener Wetteranbieter:** [Open-Meteo Forecast API](https://open-meteo.com/en/docs). Sie bietet aktuelle Bedingungen und stündliche/tägliche Vorhersagen ohne API-Schlüssel. Die kostenlose API ist für nicht-kommerzielle Nutzung vorgesehen; laut [Nutzungsbedingungen](https://open-meteo.com/en/terms) gelten unter anderem Grenzen von 10.000 Aufrufen pro Tag, 5.000 pro Stunde und 600 pro Minute. Für ein einzelnes Display mit seltenen Abrufen sollte das Kontingent ausreichen.
- Anzeige: aktuelles Wetter, Temperatur und gefühlte Temperatur, Wind und Böen, Niederschlagswahrscheinlichkeit/-menge sowie Tageshöchst- und -tiefsttemperatur für die kommenden Tage. Zeitzone: `Europe/Amsterdam`.
- WMO-Wettercodes werden in der aktuellen Wetteransicht und in der Fünf-Tage-Vorschau durch gezeichnete Symbole (Sonne, Wolken, Regen, Schnee, Nebel, Gewitter) ergänzt.
- Umgesetzte Unterseiten: aktuelle Bedingungen, Vorschau für die fünf folgenden Tage und Gezeiten mit nächstem modelliertem Hoch- oder Niedrigwasser sowie einer 12-Stunden-Verlaufsgrafik.
- **Gezeiten:** Als erste Quelle dient die [Open-Meteo Marine API](https://open-meteo.com/en/docs/marine-weather-api). Sie liefert unter anderem den modellierten Meeresspiegel einschließlich Gezeiten. Die Anzeige soll Hoch- und Niedrigwasserzeiten sowie nach Möglichkeit eine einfache Verlaufsgrafik enthalten. Der Anbieter weist darauf hin, dass die Auflösung etwa 8 km beträgt, die Genauigkeit in Küstengebieten eingeschränkt ist und die Daten nicht für Navigation geeignet sind. Werte sind daher als Modell-/Orientierungswerte zu kennzeichnen, nicht als verlässliche lokale Hochwasserwarnung.
- Open-Meteo-Daten stehen unter [CC BY 4.0](https://open-meteo.com/en/licence); auf der Wetteransicht ist die Quellenangabe „Weather data by Open-Meteo.com“ mit Link vorzusehen. Das Projekt ist laut Betreiber ausschließlich privat.
- Wetter und Gezeiten erhalten jeweils einen Aktualisierungszeitpunkt. Veraltete oder nicht verfügbare Daten werden kenntlich gemacht.
- Die lokale Uhr wird nach erfolgreicher WLAN-Verbindung über SNTP/NTP synchronisiert. Die Zeitangabe bleibt bis zur erfolgreichen Synchronisierung als nicht verfügbar gekennzeichnet.

### 5.5 Infos

- Zeigt Haus-, Ankunfts-, Abreise- und Texelhinweise aus `https://familie-riegel.de/tx94`.
- Die Seiten werden zur Build-Zeit aus `assets/info/pages.txt` als Firmware-Daten generiert. Sie stehen deshalb ohne WLAN zur Verfügung und werden durch einen Firmware-Build aktualisiert.
- Das einfache Textformat unterstützt Absätze, `- `-Listen und optional je Seite ein festes 80 x 60 RGB565-Bild. Der Generator prüft die Bildgröße und den verfügbaren Textzeilenraum.
- Seitliche Navigation blättert zwischen den erzeugten Unterseiten; der QR-Code öffnet die Quellseite der aktiven Unterseite.
- Inhalte der öffentlichen Quellseiten werden für das kleine Display knapp zusammengefasst. Private Kontakt- und Bankdaten werden nicht in die Firmware übernommen.

### 5.6 Aktualisierung und Offline-Verhalten

- Inhalte werden automatisch viermal täglich, möglichst gleichmäßig über den Tag verteilt, aktualisiert, ohne dass Gäste die Aktualisierung manuell auslösen müssen. Häufigere Abrufe sind nur für nötige Wiederholungen nach Fehlern zulässig und müssen begrenzt werden.
- Abrufe dürfen die Bedienung nicht blockieren. Fehler eines einzelnen Inhaltsbereichs dürfen die übrigen Bereiche nicht beeinträchtigen.
- Aktualisierungsintervalle sind je Quelle konfigurierbar und müssen deren Nutzungsbedingungen sowie den verfügbaren Speicher berücksichtigen.
- Letzte erfolgreiche Inhalte werden, soweit technisch und lizenzrechtlich zulässig, lokal zwischengespeichert.
- Das Display zeigt den Zeitpunkt der letzten erfolgreichen Aktualisierung und unterscheidet zwischen aktuellen, zwischengespeicherten und nicht verfügbaren Inhalten.
- WLAN-Ausfall und Serverfehler führen nicht zu einem Neustart in einer Dauerschleife oder zu einer unbedienbaren Oberfläche.
- Firmware kann OTA von `https://familie-riegel.de/tx94/firmware` aktualisiert werden. Die Firmware wird höchstens einmal pro Nacht automatisch auf eine neue Version geprüft; zusätzlich kann der Nutzer die Prüfung und Installation über die Touch-Oberfläche anstoßen.
- OTA verwendet A/B- beziehungsweise Dual-Slot-Partitionen mit fehlertolerantem Wechsel und Rückfall auf die vorherige Firmware, wenn die neue Version nicht erfolgreich startet. Firmware und Metadaten müssen über HTTPS mit Zertifikatsprüfung geladen und vor Installation auf Integrität und Echtheit geprüft werden.
- Vor Umsetzung sind das Firmware-Manifest-/Dateiformat, Signaturverfahren und der verfügbare Flashplatz zu verifizieren. Die Lösung muss mit dem 4-MB-Flash des Referenzgeräts auskommen oder die Hardwareanforderung entsprechend anpassen.

## 6. Nicht-funktionale Anforderungen

- **Lesbarkeit:** ausreichender Kontrast, gut erkennbare Schriftgrößen und keine wesentliche Information ausschließlich durch Farbe vermitteln.
- **Reaktionsfähigkeit:** Navigation und Darstellung bleiben nutzbar, während Daten geladen werden.
- **Robustheit:** zeitlich begrenzte Netzwerkzugriffe, begrenzter Speicherverbrauch und Behandlung fehlerhafter oder unvollständiger Antworten.
- **Datenschutz und Sicherheit:** keine unnötigen personenbezogenen Daten; Zugangsdaten werden nicht in versionierte Dateien oder öffentliche Artefakte eingecheckt. Der Setup-AP und das lokale HTTP-Portal sind während des Setup-Modus bewusst offen und unverschlüsselt; der Modus ist standardmäßig aus und wird nur per Touch gestartet. Er darf nur kurz in einer vertrauenswürdigen Umgebung aktiv sein. Die Auslesbarkeit der im nicht verschlüsselten NVS gespeicherten Zugangsdaten bei physischem Zugriff ist zu berücksichtigen.
- **Wartbarkeit:** Datenabruf, Datenaufbereitung und Darstellung sollen klar voneinander getrennt sein, damit sich eine geänderte Webseite nicht durch die gesamte Anzeige zieht.
- **Nachvollziehbarkeit:** Fehler und Datenalter müssen für Nutzer erkennbar sein; technische Diagnose darf keine Geheimnisse protokollieren.

## 7. Inhaltliche und technische Abhängigkeiten

- VVV Texel stellt die gewünschten Veranstaltungen und Empfehlungen in einem stabil abrufbaren Format bereit oder erlaubt deren automatisierte Nutzung.
- Die Kalender- und Infoseiten sowie der Fotoordner sind aus dem Zielnetz erreichbar und liefern ein auslesbares Format.
- Die CYD-Hardware verfügt über ein passendes Display, ausreichenden Speicher und die benötigte WLAN-Verbindung.
- Verfügbarkeit, Nutzungsbedingungen und Lizenzrechte der Inhalte und Bilder erlauben die geplante Anzeige und Zwischenspeicherung.
- Open-Meteo ist als kostenlose Wetterquelle empfohlen, vorbehaltlich der nicht-kommerziellen Nutzung und der erforderlichen Quellenangabe. Für die Marine-API sind die lokale Eignung und die Nutzungsbedingungen ebenfalls zu verifizieren.

## 8. Akzeptanzkriterien für die erste nutzbare Version

1. Das Gerät startet in eine bedienbare Oberfläche mit den fünf Bereichen Aktuell, Kalender, Fotos, Wetter und Infos. Jeder Bereich hat mehrere Unterseiten; die seitlichen Touchflächen blättern darin und die Seitenanzeige ist sichtbar.
2. Die Oberfläche ist auf dem ausgewählten CYD ohne abgeschnittene Navigation oder unlesbare Inhalte darstellbar und visuell erkennbar an der Referenz orientiert.
3. Aktuell zeigt, sofern die Quelle erreichbar ist, Termine für heute und morgen sowie sehenswerte Orte.
4. Kalender und Infos werden aus den vereinbarten Seiten dynamisch abgerufen und angezeigt, ohne Zugangsdaten in Logs offenzulegen.
5. Fotos werden aus dem vereinbarten Verzeichnis geladen und ausgewählt, sobald dort abrufbare Bilder und eine unterstützte Möglichkeit zur Dateiauswahl vorhanden sind.
6. Wetter und Gezeiten zeigen Standort/Bezugsort und Datenstand sowie aktuelle Bedingungen und die vereinbarte Vorhersage.
7. Bei unterbrochener Netzwerkverbindung bleibt die Navigation nutzbar; vorhandene letzte Daten werden als zwischengespeichert gekennzeichnet.
8. Ein Fehler oder eine leere Antwort einer Quelle verhindert nicht die Anzeige der anderen Bereiche.
9. Die Datenaktualität ist für jede dynamische Quelle erkennbar.
10. Dynamische Inhalte werden planmäßig viermal täglich aktualisiert; bei Netzwerkfehlern bleibt die Oberfläche bedienbar und zeigt den letzten Datenstand.
11. WLAN-Zugangsdaten können nach manuellem Start des Setup-Modus über das lokale Portal ohne Login konfiguriert werden. Der AP ist standardmäßig aus; das Sicherheitsrisiko des offenen HTTP-Portals wird klar ausgewiesen.
12. Ein fehlgeschlagenes OTA-Update lässt das Gerät mit der vorherigen Firmware starten.
13. Nach erfolgreicher WLAN-Verbindung wird die lokale Uhr über NTP synchronisiert und zeigt Zeit vor Datum in unterschiedlichen Farben und der Zeitzone `Europe/Amsterdam`.

## 9. Entscheidungen und offene Punkte

### Bestätigte Anforderungen

- **Hardware:** ESP32-2432S028 mit ILI9341 und XPT2046-Touch aus dem ESP_CoGo-Projekt; 320 × 240 im Querformat. Displayausrichtung und Touchbedienung wurden auf dem Gerät bestätigt. Der Touch nutzt SPI3 auf GPIO 25/32/39, unabhängig vom TFT-SPI2.
- **UI und Schrift:** LVGL 9.6 rendert mit partiellem RGB565-Framebuffer; Montserrat wird durch Noto-Sans-Glyphen für deutsche Umlaute ergänzt. Die Noto-Sans-Lizenz liegt unter `assets/fonts/OFL.txt`.
- **Interaktion:** ausschließlich Touch.
- **Framework:** ESP-IDF ist als Ziel festgelegt. Das Referenz-Repository nutzt derzeit PlatformIO/Arduino; eine Portierung beziehungsweise Übernahme nur der Hardwarekonfiguration ist erforderlich.
- **Wetterbetrieb:** ausschließlich privates Projekt; Open-Meteo Free ist damit als Anbieter vorgesehen, sofern die Nutzungsbedingungen unverändert passen. CC-BY-Quellenangabe bleibt erforderlich.
- **Gezeiten:** Hoch- und Niedrigwasserzeiten sowie möglichst eine einfache Grafik. Modellwerte der Marine API sind als Orientierung zu kennzeichnen; die Eignung und der Bezugsort auf Texel müssen noch anhand der API-Daten validiert werden.
- **Webseiten:** Kalender ist passwortgeschützt, Infos sind öffentlich. Für beide Seiten gibt es keine API; die HTML-Struktur ist stabil. Der Abruf erfolgt durch gezieltes Parsen der HTML-Antworten.
- **Fotos:** Manifestformat ist JSON (`manifest.json`) mit Pfad, ISO-Datum, Titel und Bytezahl pro Bild. Die erzeugten baseline-JPEGs sind 256 × 137 Pixel und auf 12 KiB begrenzt; Ortsnamen werden aus EXIF-GPS per Nominatim/OpenStreetMap abgeleitet.
- **Aktualisierung:** alle dynamischen Inhalte werden viermal täglich aktualisiert.
- **Sprache und Zeitzone:** ausschließlich Deutsch; Zeitzone `Europe/Amsterdam`.
- **WLAN:** Heimnetz-Zugangsdaten werden über das lokale Setup-Portal in NVS gespeichert und nicht in versionierte Dateien eingecheckt. Der Setup-AP ist standardmäßig ausgeschaltet und wird per Touch aktiviert. Währenddessen ist das WLAN offen; das Portal erfordert keinen Login und verwendet unverschlüsseltes HTTP. Der Betreiber akzeptiert dieses Verhalten für die private Verwendung und aktiviert den Setup-Modus nur kurz in vertrauenswürdiger Umgebung. Flash/NVS-Verschlüsselung ist nicht aktiviert.
- **Zeit:** NTP-Synchronisierung beginnt nach erfolgreichem Erhalt einer WLAN-IP-Adresse. Der Standort verwendet die IANA-Zeitzone `Europe/Amsterdam`.
- **Betriebsstabilität:** Netzwerkantwortpuffer müssen auf dem Heap oder in ausreichend dimensioniertem Speicher liegen; sie dürfen nicht größer als der Stack des aufrufenden Tasks sein.
- **Inhaltsrechte:** Nutzer bestätigt, dass die vorgesehene Anzeige und Zwischenspeicherung der Inhalte/Bilder gestattet ist.
- **OTA:** Firmware wird von `https://familie-riegel.de/tx94/firmware` bezogen. Prüfung erfolgt automatisch nachts oder manuell über das Display; Installation nutzt A/B-Partitionen und muss einen sicheren Rückfall ermöglichen.

### Noch zu klären/verifizieren

1. **VVV Texel:** Welche konkrete Veranstaltungs-/Tipps-Seite wird ausgelesen, und welche Kategorien oder Anzeigezeiträume sind gewünscht?
2. **Gezeiten-Daten:** Liefert die Marine API für die gewählten Texel-Koordinaten brauchbare Hoch-/Niedrigwasserzeiten? Falls nicht, muss eine verlässliche lokale Tide-Quelle gewählt werden. Die Grafik stellt den zeitlichen Modellverlauf dar und darf nicht als amtliche Vorhersage erscheinen.
3. **OTA-Paket:** Manifestformat, Versionsschema, Signatur/Prüfschlüssel und exakter Firmware-Dateipfad auf dem Server müssen festgelegt werden. Flashpartitionen und maximale Firmwaregröße sind auf dem 4-MB-Gerät nachzuweisen.
4. **Setup-Sicherheit:** Offener Setup-AP und Portal ohne Login sind für die private Nutzung bestätigt; das verbleibende Risiko bei länger aktivem Setup-Modus ist zu beachten.

## 10. Vorgeschlagene Umsetzungsreihenfolge

1. Zielhardware, ESP-IDF-Projektbasis und Displayausrichtung am Gerät verifizieren.
2. WLAN-Setup und geschützte Konfiguration, Grundlayout und Touch-Navigation umsetzen.
3. Wetter und Gezeiten einschließlich Modellhinweis und Grafik integrieren.
4. Kalender und öffentliche Hausinformationen aus der stabilen HTML-Struktur abrufen und darstellen.
5. VVV-Termine/Orte und Foto-Manifest integrieren.
6. OTA-Manifest, Signaturprüfung, A/B-Partitionen und Rollback auf dem Gerät testen.
7. Offline-Verhalten, Aktualitätsanzeige, Fehlerfälle und Anzeige auf dem echten CYD testen.
