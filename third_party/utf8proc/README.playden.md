# utf8proc provenance

The four upstream files in this directory are unmodified from JuliaStrings/utf8proc v2.11.3, commit `e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78`. `dependencies.lock.json` records their SHA-256 hashes, checked at configure time. The library is compiled directly into the host and PS5 core so it needs no platform locale APIs or companion service.

Upstream source: https://github.com/JuliaStrings/utf8proc/tree/v2.11.3
API documentation: https://juliastrings.github.io/utf8proc/doc/utf8proc_8h.html

Playden uses Unicode case folding for deterministic cached-title search. Sorting compares the folded UTF-8 bytes, with stable app IDs breaking ties; it is not language-specific collation. Accent stripping and canonical normalization are not performed. Non-Latin substrings remain searchable. See LICENSE.md for the MIT and Unicode data notices.
