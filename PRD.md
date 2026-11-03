# Product Requirements Document: Texel-Info-Display

**Status:** Entwurf  
**Produkt:** ESP32 CYD-Info-Display für ein Ferienhaus auf Texel  
**Sprache der Anzeige:** Deutsch  
**Stand:** 3. November 2026

## 1. Produktidee

Ein dauerhaft verfügbares, auf einen ESP32 CYD (Cheap Yellow Display) zugeschnittenes Informationsdisplay für Gäste eines Ferienhauses auf Texel. Die Oberfläche orientiert sich am bereitgestellten Screenshot: dunkles Design, orangefarbene Akzente, kompakte Registerkarten und eine gut lesbare Informationsfläche.

Die Inhalte werden, soweit möglich, regelmäßig aus den angegebenen Webquellen aktualisiert. Das Display soll auch bei vorübergehendem Ausfall von WLAN oder Datenquellen sinnvoll nutzbar bleiben und den letzten erfolgreichen Datenstand anzeigen.

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

- Zielplattform laut [ESP_CoGo-Projekt](https://github.com/JanPeter1/ESP_CoGo): ESP32-2432S028, ESP32 Dev Module, 2,8-Zoll-ILI9341-Farbdisplay (320 × 240 Pixel in der Projektbeschreibung; Setup definiert 240 × 320) und XPT2046-resistiver Touchcontroller.
- Ziel-Framework ist ESP-IDF. Das Referenz-Repository ist derzeit allerdings auf PlatformIO mit Arduino-Framework konfiguriert; die Hardwarekonfiguration kann als Vorlage dienen, die Firmware muss für ESP-IDF portiert werden.
- Die dortige TFT_eSPI-Konfiguration verwendet unter anderem Hintergrundbeleuchtung GPIO 21 und Touch-CS GPIO 33. Pinbelegung und Displayorientierung sind vor Übernahme auf dem konkreten Gerät zu verifizieren.
- Das Layout soll sich am angehängten Referenzbild orientieren: dunkler Hintergrund, kontrastreiche Texte, orange Hervorhebungen, kompakte Navigationsleiste und Karten-/Listenansichten.
- Hauptbereiche:
  1. **Aktuell**
  2. **Kalender**
  3. **Fotos**
  4. **Wetter**
  5. **Infos**
- Jeder Hauptbereich besitzt mehrere Unterseiten. Die seitlichen `<`- und `>`-Touchflächen blättern ausschließlich innerhalb des ausgewählten Bereichs; eine Seitenanzeige zeigt die aktuelle Position. Ein Wechsel des Hauptbereichs startet auf dessen erster Unterseite.
- Bedienung ausschließlich über den XPT2046-Touchcontroller; vor Umsetzung ist die Touchkalibrierung auf dem konkreten Gerät zu prüfen.
- Die Oberfläche muss für die tatsächliche Displayauflösung und Orientierung des ausgewählten Geräts optimiert werden. Der Screenshot dient als visuelle Referenz, nicht als verbindliche Pixelvorlage.
- Lange Inhalte müssen scrollbar oder in kompakte, einzeln navigierbare Karten aufgeteilt sein. Text darf nicht abgeschnitten werden, ohne dass er erreichbar ist.

## 5. Funktionale Anforderungen

### 5.1 Aktuell

- Zeigt aktuelle Veranstaltungen und Termine auf Texel, mindestens für heute und morgen.
- Quelle: Webseite des VVV Texel.
- Zeigt außerdem sehenswerte Orte und Tipps; die Referenzoberfläche sieht Filter/Kategorien wie „Alle“, „Orte & Sights“ und „Kinder & Familie“ sowie eine Karten-/Karussellansicht vor.
- Jeder Eintrag soll, soweit von der Quelle geliefert, Titel, Datum/Uhrzeit, Ort und eine kurze Beschreibung enthalten.
- Wenn Inhalte nicht abrufbar sind, werden gespeicherte letzte Inhalte mit ihrem Aktualisierungszeitpunkt gezeigt. Sind keine gespeicherten Inhalte vorhanden, erscheint eine verständliche Fehlermeldung.

### 5.2 Kalender

- Zeigt den Texelkalender, dynamisch bezogen von `https://familie-riegel.de/tx94/?Texel-Kalender`.
- Der Abruf erfordert laut Nutzerangabe eine Anmeldung. Eine API ist nicht vorhanden; die festgelegte HTML-Struktur wird automatisiert abgerufen und gezielt geparst.
- Kalenderinhalte werden lesbar und passend zum kleinen Display dargestellt, zum Beispiel als Tages-/Terminliste.
- Der geschützte Kalender wird mit den vom Betreiber bereitgestellten Zugangsdaten abgerufen. Zugangsdaten werden nicht in versionierte Dateien oder öffentliche Artefakte geschrieben, sondern bei der lokalen Bereitstellung über eine nicht versionierte Konfiguration beziehungsweise Gerätekonfiguration eingebracht. Im Firmware-Image oder Gerätespeicher hinterlegte Zugangsdaten sind bei physischem Zugriff möglicherweise auslesbar.

### 5.3 Fotos

- Zeigt zufällig ausgewählte Bilder aus `https://familie-riegel.de/tx94/photos/320x200`.
- Die Bildquelle wird laut Nutzerangabe noch eingerichtet.
- Empfohlenes Bildformat und -maß: JPEG mit 320 × 200 Pixeln, passend zum vorgesehenen Fotoverzeichnis. Zielgröße möglichst höchstens 100 KB pro Bild; die konkrete Qualität ist gegen sichtbare Artefakte und Downloadzeit zu prüfen.
- Bilder werden ohne Verzerrung angezeigt; bei mehreren verfügbaren Bildern erfolgt eine zufällige Auswahl beziehungsweise Rotation.
- Der Abruf muss mit dem Speicher- und Bandbreitenbudget des ESP32 vereinbar sein. Fehlerhafte oder nicht unterstützte Bilder dürfen die übrige Oberfläche nicht blockieren.
- Vor Umsetzung ist zu klären, wie die Bilddateien in diesem Pfad aufgelistet werden können (Indexseite, Manifest oder API).
- eventuell hier auch eine einbindung von webcams auf texel (zu finden auf der Webseite des VVV Texel).

### 5.4 Wetter und Gezeiten

- **Standort:** Slufterweg 94, De Cocksdorp, Texel. Für die Abfrage ermittelte OpenStreetMap/Nominatim die Koordinaten **53.1242788° N, 4.8128299° E**. Die Adresse muss nicht auf dem Display erscheinen.
- **Empfohlener Wetteranbieter:** [Open-Meteo Forecast API](https://open-meteo.com/en/docs). Sie bietet aktuelle Bedingungen und stündliche/tägliche Vorhersagen ohne API-Schlüssel. Die kostenlose API ist für nicht-kommerzielle Nutzung vorgesehen; laut [Nutzungsbedingungen](https://open-meteo.com/en/terms) gelten unter anderem Grenzen von 10.000 Aufrufen pro Tag, 5.000 pro Stunde und 600 pro Minute. Für ein einzelnes Display mit seltenen Abrufen sollte das Kontingent ausreichen.
- Anzeige: aktuelles Wetter, Temperatur und gefühlte Temperatur, Wind und Böen, Niederschlagswahrscheinlichkeit/-menge sowie Tageshöchst- und -tiefsttemperatur für die kommenden Tage. Zeitzone: `Europe/Amsterdam`.
- **Gezeiten:** Als erste Quelle dient die [Open-Meteo Marine API](https://open-meteo.com/en/docs/marine-weather-api). Sie liefert unter anderem den modellierten Meeresspiegel einschließlich Gezeiten. Die Anzeige soll Hoch- und Niedrigwasserzeiten sowie nach Möglichkeit eine einfache Verlaufsgrafik enthalten. Der Anbieter weist darauf hin, dass die Auflösung etwa 8 km beträgt, die Genauigkeit in Küstengebieten eingeschränkt ist und die Daten nicht für Navigation geeignet sind. Werte sind daher als Modell-/Orientierungswerte zu kennzeichnen, nicht als verlässliche lokale Hochwasserwarnung.
- Open-Meteo-Daten stehen unter [CC BY 4.0](https://open-meteo.com/en/licence); auf der Wetteransicht ist die Quellenangabe „Weather data by Open-Meteo.com“ mit Link vorzusehen. Das Projekt ist laut Betreiber ausschließlich privat.
- Wetter und Gezeiten erhalten jeweils einen Aktualisierungszeitpunkt. Veraltete oder nicht verfügbare Daten werden kenntlich gemacht.

### 5.5 Infos

- Zeigt Informationen von `https://familie-riegel.de/tx94`.
- Inhalte werden dynamisch aktualisiert und für das Display lesbar aufbereitet.
- Die Informationen sind ohne Anmeldung zugänglich. Welche konkreten Seitenbereiche einzubeziehen sind, wird bei der Implementierung anhand der stabilen HTML-Struktur festgelegt.
- Bei längeren Inhalten muss Navigation/Scrollen möglich sein; externe Links können auf dem Gerät als URL oder QR-Code dargestellt werden, sofern sinnvoll.

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
- **Datenschutz und Sicherheit:** keine unnötigen personenbezogenen Daten; Zugangsdaten werden nicht in versionierte Dateien oder öffentliche Artefakte eingecheckt und nicht unverschlüsselt übertragen. Die lokale Bereitstellung von privaten WLAN- und Kalender-Zugangsdaten ist möglich; die Auslesbarkeit aus Firmware beziehungsweise Gerätespeicher ist zu berücksichtigen.
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
11. WLAN-Zugangsdaten können über die geschützte lokale Konfiguration ergänzt werden, ohne sie in versionierten Dateien abzulegen.
12. Ein fehlgeschlagenes OTA-Update lässt das Gerät mit der vorherigen Firmware starten.

## 9. Entscheidungen und offene Punkte

### Bestätigte Anforderungen

- **Hardware:** ESP32-2432S028 mit ILI9341 und XPT2046-Touch aus dem ESP_CoGo-Projekt. Die widersprüchlichen Orientierungs-/Auflösungsangaben (320 × 240 in der Beschreibung, 240 × 320 im Display-Setup) werden vor dem UI-Layout am realen Gerät geprüft.
- **Interaktion:** ausschließlich Touch.
- **Framework:** ESP-IDF ist als Ziel festgelegt. Das Referenz-Repository nutzt derzeit PlatformIO/Arduino; eine Portierung beziehungsweise Übernahme nur der Hardwarekonfiguration ist erforderlich.
- **Wetterbetrieb:** ausschließlich privates Projekt; Open-Meteo Free ist damit als Anbieter vorgesehen, sofern die Nutzungsbedingungen unverändert passen. CC-BY-Quellenangabe bleibt erforderlich.
- **Gezeiten:** Hoch- und Niedrigwasserzeiten sowie möglichst eine einfache Grafik. Modellwerte der Marine API sind als Orientierung zu kennzeichnen; die Eignung und der Bezugsort auf Texel müssen noch anhand der API-Daten validiert werden.
- **Webseiten:** Kalender ist passwortgeschützt, Infos sind öffentlich. Für beide Seiten gibt es keine API; die HTML-Struktur ist stabil. Der Abruf erfolgt durch gezieltes Parsen der HTML-Antworten.
- **Fotos:** Es wird ein Manifest für die Bilddateien bereitgestellt. Empfohlen sind JPEGs mit 320 × 200 Pixeln und möglichst höchstens 100 KB pro Bild.
- **Aktualisierung:** alle dynamischen Inhalte werden viermal täglich aktualisiert.
- **Sprache und Zeitzone:** ausschließlich Deutsch; Zeitzone `Europe/Amsterdam`.
- **WLAN:** ein vorgegebenes Heimnetz wird beim lokalen Gerätesetup eingerichtet. Weitere WLAN-Netze können über eine geschützte lokale Webkonfiguration ergänzt werden. SSID und Passwörter werden nicht in versionierte Dateien oder öffentliche Artefakte eingecheckt; die Geräteoberfläche muss Zugangsdaten schützen.
- **Inhaltsrechte:** Nutzer bestätigt, dass die vorgesehene Anzeige und Zwischenspeicherung der Inhalte/Bilder gestattet ist.
- **OTA:** Firmware wird von `https://familie-riegel.de/tx94/firmware` bezogen. Prüfung erfolgt automatisch nachts oder manuell über das Display; Installation nutzt A/B-Partitionen und muss einen sicheren Rückfall ermöglichen.

### Noch zu klären/verifizieren

1. **VVV Texel:** Welche konkrete Veranstaltungs-/Tipps-Seite wird ausgelesen, und welche Kategorien oder Anzeigezeiträume sind gewünscht?
2. **Gezeiten-Daten:** Liefert die Marine API für die gewählten Texel-Koordinaten brauchbare Hoch-/Niedrigwasserzeiten? Falls nicht, muss eine verlässliche lokale Tide-Quelle gewählt werden. Die Grafik stellt den zeitlichen Modellverlauf dar und darf nicht als amtliche Vorhersage erscheinen.
3. **Manifest:** Dateiname, URL, Format und Aktualisierung des Foto-Manifests müssen festgelegt werden.
4. **OTA-Paket:** Manifestformat, Versionsschema, Signatur/Prüfschlüssel und exakter Firmware-Dateipfad auf dem Server müssen festgelegt werden. Flashpartitionen und maximale Firmwaregröße sind auf dem 4-MB-Gerät nachzuweisen.
5. **WLAN-Konfiguration:** Der Zugang zur lokalen Konfigurationsoberfläche (zum Beispiel Setup-Modus oder lokales Admin-Passwort) muss festgelegt und gegen unbefugte Änderung geschützt werden.

## 10. Vorgeschlagene Umsetzungsreihenfolge

1. Zielhardware, ESP-IDF-Projektbasis und Displayausrichtung am Gerät verifizieren.
2. WLAN-Setup und geschützte Konfiguration, Grundlayout und Touch-Navigation umsetzen.
3. Wetter und Gezeiten einschließlich Modellhinweis und Grafik integrieren.
4. Kalender und öffentliche Hausinformationen aus der stabilen HTML-Struktur abrufen und darstellen.
5. VVV-Termine/Orte und Foto-Manifest integrieren.
6. OTA-Manifest, Signaturprüfung, A/B-Partitionen und Rollback auf dem Gerät testen.
7. Offline-Verhalten, Aktualitätsanzeige, Fehlerfälle und Anzeige auf dem echten CYD testen.
