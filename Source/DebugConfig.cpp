/*
==============================================================================
DebugConfig.cpp
------------------------------------------------------------------------------
Role du fichier:
- Initialiser le logger global du plugin.
- Fournir une sortie de logs hybride (JUCE logger + fichier + os_log).
- Dedupliquer les logs d etat repetitifs pour limiter le bruit.

Place dans l architecture:
- Utilitaire transversal utilise par tous les modules via DBG_LOG().
- Aucune logique audio/metier ici.

Contraintes:
- Si LOGS_ENABLED == 0, tout doit se compiler en no-op propre.
- Ce logger ne doit pas etre appele de maniere intensive en thread audio.
==============================================================================
*/

#include "DebugConfig.h"
//==============================================================================
// ID MAP – LOGS : MODULE UI → InputFilter
//==============================================================================
// TYPE        MODULE        CONTEXT              ID      DESCRIPTION
//------------------------------------------------------------------------------
// STATE       INPUTFILTER   GLOBAL ENABLE        IF000   Activation globale du filtre
// STATE       INPUTFILTER   MUTE                 IF001   Toggle Mute ON/OFF
// STATE       INPUTFILTER   CHANNEL CHANGE       IF002   Sélection du canal MIDI
// STATE       INPUTFILTER   NOTE TOGGLE          IF003   Toggle du filtre de notes
// STATE       INPUTFILTER   NOTE SLIDER          IF004   Valeurs min/max du slider de notes
// STATE       INPUTFILTER   VELO TOGGLE          IF005   Toggle du filtre de vélocité
// STATE       INPUTFILTER   VELO SLIDER          IF006   Valeurs min/max du slider de vélocité
// STATE       INPUTFILTER   STEP TOGGLE          IF007   Toggle du filtre N/M
// STATE       INPUTFILTER   STEP SLIDER          IF008   Valeurs N/M du filtre de pas
// STATE       INPUTFILTER   VOICE TOGGLE         IF009   Toggle de la voice limit
// STATE       INPUTFILTER   VOICE SLIDER         IF010   Valeur du slider de voice limit
// STATE       INPUTFILTER   PRIORITY             IF011   Changement de priorité
//==============================================================================
// ID MAP – LOGS : MODULE PROCESSOR → InputFilterProcessor
//==============================================================================
// TYPE        MODULE        CONTEXT              ID      DESCRIPTION
//------------------------------------------------------------------------------
// STATE       INPUTFILTER   PROCESS BLOCK        IFP00   Entrée de process()
// STATE       INPUTFILTER   CHANNEL CHANGE       IFP01   Changement de canal MIDI
// STATE       INPUTFILTER   MUTE                 IFP02   Mute actif → All Notes Off
// STATE       INPUTFILTER   NOTE FILTER          IFP03   Statut ON/OFF + plage min/max
// STATE       INPUTFILTER   VELO FILTER          IFP04   Statut ON/OFF + plage min/max
// STATE       INPUTFILTER   VOICE LIMIT          IFP05   ON/OFF + max voices + priorité
// STATE       INPUTFILTER   STEP FILTER          IFP06   ON/OFF + N/M + stepCount

// ACTION      INPUTFILTER   NOTE ON ACCEPTED     IFP10   Note acceptée (passage OK)
// ACTION      INPUTFILTER   NOTE OFF ACCEPTED    IFP11   NoteOff autorisé et transmis
// ACTION      INPUTFILTER   NOTE BLOCKED         IFP12   Note bloquée (mute / note / velo / step)
// ACTION      INPUTFILTER   NOTE BLOCKED VOICE   IFP13   Note bloquée par Voice Limit
// ACTION      INPUTFILTER   ALL NOTES OFF        IFP14   Envoi All Notes Off manuel ou automatique
//==============================================================================

// Logger unique pour toute la session plugin.
static HybridLogger globalHybridLogger;

struct HybridLoggerInitializer
{
    HybridLoggerInitializer()
    {
        // Branche ce logger comme logger JUCE courant des le chargement du binaire.
        juce::Logger::setCurrentLogger(&globalHybridLogger);
        DBG("======================================");
        DBG("======================================");
        DBG("MDVZ    INITIALISATION        PLUGIN PROCESSOR     STARTUP              #000#    Logger initialised - New Session");
    }
    ~HybridLoggerInitializer()
    {
        // Nettoyage defensif en shutdown.
        juce::Logger::setCurrentLogger(nullptr);
    }
};

static HybridLoggerInitializer globalHybridLoggerInitializer;

// === HybridLogger implementation ===
HybridLogger::HybridLogger()
{
#if LOGS_ENABLED
    // Chemin historique du projet: conserve pour ne pas casser la routine
    // actuelle de debug local.
    juce::File logDir ("/Users/jonathanblacas/Dev/MidivisiVici_Builds/MacOSX/build");
    logDir.createDirectory();

    auto today = juce::Time::getCurrentTime();
    auto fileName = today.formatted("%Y.%m.%d") + ".log.txt";

    logFile = logDir.getChildFile(fileName);
    fileStream = logFile.createOutputStream(4096);

    if (fileStream != nullptr)
        fileStream->setPosition(logFile.getSize());
#endif
}

void HybridLogger::logMessage(const juce::String& message)
{
#if !LOGS_ENABLED
    juce::ignoreUnused(message);
    return;
#else
    // Formattage unique pour toutes les cibles de log.
    auto timestamp = juce::Time::getCurrentTime().formatted("%H:%M:%S");
    juce::String fullMessage = "[" + timestamp + "] " + message;
    auto* utf8 = fullMessage.toRawUTF8();

    if (fileStream != nullptr)
    {
        fileStream->write(utf8, (int) std::strlen(utf8));
        fileStream->write("\n", 1);
        fileStream->flush();
    }

    std::cout << utf8 << std::endl; // console locale
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[MDVZ] %{public}s", utf8);
#endif
}

void HybridLogger::logFormatted(const juce::String& type,
                                const juce::String& module,
                                const juce::String& context,
                                const juce::String& id,
                                const juce::String& message)
{
#if !LOGS_ENABLED
    juce::ignoreUnused(type, module, context, id, message);
    return;
#else
    // formatFixed garde un layout de colonnes stable pour lecture rapide.
    auto formatFixed = [] (juce::String text, int width)
    {
        if (text.length() > width)
            return text.substring(0, width);
        while (text.length() < width)
            text += " ";
        return text;
    };

    // Cle d identite (sans message) pour regrouper les etats repetitifs.
    juce::String stateKey = "MDVZ    "
        + formatFixed(type.toUpperCase(),   20) + "    "
        + formatFixed(module.toUpperCase(), 20) + "    "
        + formatFixed(context,              20) + "    "
        + id;

    // Cle complete = identite + payload.
    juce::String fullState = stateKey + "    " + message;

    // Memo par clef:
    // Chaque (type,module,context,id) conserve son dernier message pour dedup.
    static std::unordered_map<std::string, juce::String> lastStates;

    auto& lastMsg = lastStates[stateKey.toStdString()];
    if (lastMsg == fullState)
        return; // rien n'a change -> skip
    lastMsg = fullState;

    // Ajout timestamp uniquement pour affichage
    auto timestamp = juce::Time::getCurrentTime().formatted("[%H:%M:%S] ");
    juce::String finalMsg = timestamp + fullState;

    juce::Logger::writeToLog(finalMsg);
#endif
}
