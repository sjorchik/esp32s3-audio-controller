#pragma once

// Абстракція «джерело рівня VU».
// Потрібна, щоб пізніше додати PCM1808 без переробки UI.
// У версії 1 VU рахується з декодованих PCM-семплів
// і працює лише в режимі радіо.

#include <stdint.h>

class VuSource {
public:
    virtual ~VuSource() = default;

    // Прочитати поточні рівні.
    // Повертає нормалізовані значення 0..1.
    virtual bool read(float& left, float& right) = 0;
};

// Джерело з декодованих PCM-семплів радіо.
class VuSourceDecodedPcm : public VuSource {
public:
    bool read(float& left, float& right) override;
};