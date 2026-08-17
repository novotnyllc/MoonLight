/**
    @title     MoonLight
    @file      L_WhiteVest95.h
    @repo      https://github.com/novotnyllc/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
**/

#pragma once

#if FT_MOONLIGHT

class WhiteVest95Layout : public Node {
 public:
  static const char* name() { return "White Vest 95"; }
  static uint8_t dim() { return _3D; }
  static const char* tags() { return "🚥"; }
  static const char* category() { return "Layout"; }

  Coord3D vestCoord(uint32_t x, uint32_t y, uint32_t z) const {
    return Coord3D((x * 5U + 127U) / 255U, (y * 5U + 127U) / 255U, (z * 19U + 127U) / 255U);
  }

  bool hasOnLayout() const override { return true; }
  void onLayout() override {
    addLight(vestCoord(44, 31, 40));
    addLight(vestCoord(44, 31, 65));
    addLight(vestCoord(44, 31, 90));
    addLight(vestCoord(44, 31, 127));
    addLight(vestCoord(44, 31, 164));
    addLight(vestCoord(44, 31, 213));
    addLight(vestCoord(44, 31, 250));
    addLight(vestCoord(15, 68, 248));
    addLight(vestCoord(15, 68, 212));
    addLight(vestCoord(15, 68, 194));
    addLight(vestCoord(15, 68, 147));
    addLight(vestCoord(15, 68, 118));
    addLight(vestCoord(15, 68, 82));
    addLight(vestCoord(15, 68, 102));
    addLight(vestCoord(15, 68, 15));
    addLight(vestCoord(1, 113, 30));
    addLight(vestCoord(1, 113, 56));
    addLight(vestCoord(1, 113, 81));
    addLight(vestCoord(1, 113, 116));
    addLight(vestCoord(1, 113, 151));
    addLight(vestCoord(1, 113, 191));
    addLight(vestCoord(1, 113, 208));
    addLight(vestCoord(4, 160, 174));
    addLight(vestCoord(4, 160, 146));
    addLight(vestCoord(4, 160, 110));
    addLight(vestCoord(4, 160, 73));
    addLight(vestCoord(4, 160, 52));
    addLight(vestCoord(4, 160, 28));
    addLight(vestCoord(4, 160, 20));
    addLight(vestCoord(25, 203, 53));
    addLight(vestCoord(25, 203, 82));
    addLight(vestCoord(25, 203, 103));
    addLight(vestCoord(25, 203, 139));
    addLight(vestCoord(25, 203, 182));
    addLight(vestCoord(25, 203, 224));
    addLight(vestCoord(25, 203, 253));
    addLight(vestCoord(60, 235, 248));
    addLight(vestCoord(60, 235, 210));
    addLight(vestCoord(60, 235, 165));
    addLight(vestCoord(60, 235, 124));
    addLight(vestCoord(60, 235, 87));
    addLight(vestCoord(60, 235, 64));
    addLight(vestCoord(60, 235, 37));
    addLight(vestCoord(104, 253, 48));
    addLight(vestCoord(104, 253, 73));
    addLight(vestCoord(104, 253, 104));
    addLight(vestCoord(104, 253, 143));
    addLight(vestCoord(104, 253, 189));
    addLight(vestCoord(104, 253, 233));
    addLight(vestCoord(104, 253, 245));
    addLight(vestCoord(151, 253, 251));
    addLight(vestCoord(151, 253, 214));
    addLight(vestCoord(151, 253, 168));
    addLight(vestCoord(151, 253, 124));
    addLight(vestCoord(151, 253, 81));
    addLight(vestCoord(151, 253, 54));
    addLight(vestCoord(151, 253, 27));
    addLight(vestCoord(195, 235, 40));
    addLight(vestCoord(195, 235, 69));
    addLight(vestCoord(195, 235, 106));
    addLight(vestCoord(195, 235, 148));
    addLight(vestCoord(195, 235, 191));
    addLight(vestCoord(195, 235, 240));
    addLight(vestCoord(195, 235, 234));
    addLight(vestCoord(230, 203, 253));
    addLight(vestCoord(230, 203, 223));
    addLight(vestCoord(230, 203, 181));
    addLight(vestCoord(230, 203, 163));
    addLight(vestCoord(230, 203, 121));
    addLight(vestCoord(230, 203, 81));
    addLight(vestCoord(230, 203, 55));
    addLight(vestCoord(230, 203, 20));
    addLight(vestCoord(251, 160, 64));
    addLight(vestCoord(251, 160, 95));
    addLight(vestCoord(251, 160, 138));
    addLight(vestCoord(251, 160, 156));
    addLight(vestCoord(254, 113, 116));
    addLight(vestCoord(254, 113, 77));
    addLight(vestCoord(254, 113, 40));
    addLight(vestCoord(254, 113, 6));
    addLight(vestCoord(240, 68, 65));
    addLight(vestCoord(240, 68, 90));
    addLight(vestCoord(240, 68, 105));
    addLight(vestCoord(240, 68, 135));
    addLight(vestCoord(240, 68, 169));
    addLight(vestCoord(240, 68, 184));
    addLight(vestCoord(240, 68, 195));
    addLight(vestCoord(240, 68, 248));
    addLight(vestCoord(211, 31, 240));
    addLight(vestCoord(211, 31, 209));
    addLight(vestCoord(211, 31, 164));
    addLight(vestCoord(211, 31, 118));
    addLight(vestCoord(211, 31, 85));
    addLight(vestCoord(211, 31, 65));
    addLight(vestCoord(211, 31, 22));
    nextPin();
  }
};

#endif  // FT_MOONLIGHT
