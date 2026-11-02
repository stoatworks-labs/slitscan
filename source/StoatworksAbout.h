/*
 * Stoatworks Labs - About window data for Slitscan.
 *
 * A PROVISIONAL HAND COPY, adapted from slitscan's on 2026-11-02: slitscan is
 * not in the website's projects.json yet, so sync-about.py has nothing to
 * generate it from. The first sync after registration overwrites it. Until
 * then there is no guide (no User guide button), and the page and repo links
 * name the homes the project is meant to have.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Slitscan";
    inline constexpr auto slug = "slitscan";
    inline constexpr auto hook = "Slit-scan and strip photography, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/slitscan/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/slitscan";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
