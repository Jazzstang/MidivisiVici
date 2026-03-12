/*
==============================================================================
MorphParameterManager.cpp
------------------------------------------------------------------------------
Implementation note:
- La majeure partie de MorphParameterManager est volontairement inline dans
  le header pour garder une API simple et permettre une optimisation aggressive
  des acces atomiques (petites fonctions appelees tres souvent).

Ce fichier existe pour:
- fournir une unite de compilation dediee,
- garder une structure projet symetrique (.h/.cpp),
- permettre des extensions futures sans casser l organisation.
==============================================================================
*/

#include "MorphParameterManager.h"
