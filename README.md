# Simple Host (Prisma Audio)

Πολύ απλός standalone host για plugins VST3 και AU, για Mac με Apple Silicon.

## Τι κάνει
- Φορτώνει ένα plugin κάθε φορά και ανοίγει το παράθυρό του.
- Δέχεται MIDI από τους συνδεδεμένους controllers και στέλνει τον ήχο στην έξοδο που διαλέγεις.
- Θυμάται το τελευταίο plugin, τις ρυθμίσεις του και τις ρυθμίσεις ήχου.

## Πώς το κατεβάζεις
1. Στο GitHub άνοιξε την καρτέλα **Actions** και διάλεξε το τελευταίο πράσινο run.
2. Κατέβασε το **SimpleHost-macOS** από το κάτω μέρος της σελίδας και ξεζίπαρέ το δύο φορές (το GitHub ζιπάρει και το zip).
3. Σύρε το `Simple Host.app` στον φάκελο Applications.

## Πρώτο άνοιγμα
Η εφαρμογή δεν είναι υπογεγραμμένη από την Apple. Την πρώτη φορά κάνε δεξί κλικ → **Open** → **Open**.
Αν το macOS την μπλοκάρει, τρέξε στο Terminal:

```
xattr -cr "/Applications/Simple Host.app"
```

## Χρήση
1. **Φόρτωση plugin…** και διάλεξε ένα `.vst3` από `/Library/Audio/Plug-Ins/VST3` ή ένα `.component` από `/Library/Audio/Plug-Ins/Components`.
2. **Ρυθμίσεις ήχου και MIDI…** για έξοδο ήχου και MIDI συσκευές.

## Περιορισμοί
- Μόνο plugins που έχουν έκδοση για Apple Silicon (arm64 ή universal).
- Ένα plugin κάθε φορά.
