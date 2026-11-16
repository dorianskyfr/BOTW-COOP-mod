#include "svc_logger.hpp"

#include <algorithm>
#include <cstring>
#include <common.hpp>
#include <program/setting.hpp>

/*
 * Remplace le svc_logger.cpp d'exlaunch (plugin/build.sh recopie l'overlay par-dessus) : chaque message
 * finit par un saut de ligne. Eden (derive de yuzu) garde le texte de svcOutputDebugString jusqu'au
 * prochain saut de ligne : sans lui, les messages du mod sortaient par paquets de ~4 Ko, en retard et
 * colles les uns aux autres (essai e4 du 2026-11-16). Ryujinx ecrit chaque appel sur sa propre ligne
 * et ne fait qu'afficher ce saut de ligne en plus.
 */
namespace exl::log {

    void SvcLogger::LogRaw(std::string_view string) {
        char buffer[setting::LogBufferSize + 1];
        const std::size_t length = std::min(string.size(), sizeof(buffer) - 1);
        std::memcpy(buffer, string.data(), length);
        buffer[length] = '\n';
        svcOutputDebugString(buffer, length + 1);
    }
}
