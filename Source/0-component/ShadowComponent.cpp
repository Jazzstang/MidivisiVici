/*
==============================================================================
ShadowComponent.cpp
------------------------------------------------------------------------------
Role du fichier:
- Composant decoratif reutilisable pour dessiner une zone "ombre/fond" autour
  d un contenu enfant.

Etat actuel du projet:
- Le mode flat performance desactive le rendu d ombre dans paint().
- Le composant reste utile pour:
  - definir une zone interne via margin,
  - gerer le hitTest (zone cliquable),
  - conserver une API stable pour les composants qui l utilisent.

Invariants:
- cornerRadius < 0 active un rayon dynamique type "pilule".
- hitTest s appuie toujours sur la shadow area reduite.

Threading:
- UI thread uniquement.
==============================================================================
*/

#include "ShadowComponent.h"

ShadowComponent::ShadowComponent(const juce::DropShadow& ds,
                                 float r,
                                 bool tl, bool tr,
                                 bool bl, bool br,
                                 int m)
    : shadow(ds),
      cornerRadius(r),  // <0 = mode pilule dynamique
      topLeft(tl), topRight(tr),
      bottomLeft(bl), bottomRight(br),
      margin(m)
{
    setInterceptsMouseClicks(false, false);
}

void ShadowComponent::setShadow(const juce::DropShadow& ds)
{
    shadow = ds;
    repaint();
}

void ShadowComponent::setCornerRadii(float r,
                                     bool tl, bool tr,
                                     bool bl, bool br)
{
    cornerRadius = r;
    topLeft = tl;
    topRight = tr;
    bottomLeft = bl;
    bottomRight = br;
    repaint();
}

void ShadowComponent::setMargin(int m)
{
    margin = m;
    repaint();
}

juce::Rectangle<int> ShadowComponent::getShadowArea() const
{
    return getLocalBounds().reduced(margin);
}

void ShadowComponent::addSelectiveRoundedRect(juce::Path& path,
                                              juce::Rectangle<float> area) const
{
    const float x = area.getX();
    const float y = area.getY();
    const float w = area.getWidth();
    const float h = area.getHeight();

    // Rayon dynamique:
    // - negatif => capsule automatique basee sur la taille courante.
    const float r = (cornerRadius < 0.0f)
                        ? juce::jmin(w, h) * 0.5f
                        : juce::jmin(cornerRadius, juce::jmin(w, h) * 0.5f);

    path.startNewSubPath(x + (topLeft ? r : 0), y);

    // Top edge
    if (topRight)
    {
        path.lineTo(x + w - r, y);
        path.quadraticTo(x + w, y, x + w, y + r);
    }
    else
        path.lineTo(x + w, y);

    // Right edge
    if (bottomRight)
    {
        path.lineTo(x + w, y + h - r);
        path.quadraticTo(x + w, y + h, x + w - r, y + h);
    }
    else
        path.lineTo(x + w, y + h);

    // Bottom edge
    if (bottomLeft)
    {
        path.lineTo(x + r, y + h);
        path.quadraticTo(x, y + h, x, y + h - r);
    }
    else
        path.lineTo(x, y + h);

    // Left edge
    if (topLeft)
    {
        path.lineTo(x, y + r);
        path.quadraticTo(x, y, x + r, y);
    }
    else
        path.lineTo(x, y);

    path.closeSubPath();
}

void ShadowComponent::paint(juce::Graphics& g)
{
    juce::ignoreUnused(g);
    // Flat performance mode: ombres desactivees globalement.
    // On conserve ce composant comme support de layout/hit area.
}

bool ShadowComponent::hitTest(int x, int y)
{
    return getShadowArea().contains(x, y);
}
