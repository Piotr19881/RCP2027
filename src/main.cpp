#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <MFRC522.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <Wire.h>
#include <cstring>
#include <cmath>

#include <vector>

#include "pins.h"

namespace {

constexpr uint16_t kScreenWidth = 480;
constexpr uint16_t kScreenHeight = 320;
constexpr uint32_t kRelayPulseMs = 3000;
constexpr uint32_t kRfidDebounceMs = 1400;
constexpr uint32_t kClockRefreshMs = 1000;
constexpr uint32_t kAttendanceConfirmationMs = 3000;
constexpr uint32_t kHttpTimeoutMs = 8000;
constexpr uint32_t kEmployeeRefreshMs = 300000;
constexpr uint8_t kMaximumEmployees = 180;
constexpr char kConfigPath[] = "/config.json";
constexpr char kEmployeesPath[] = "/employees.csv";
constexpr char kAttendancePath[] = "/attendance.csv";
constexpr char kLogsPath[] = "/logs.csv";
constexpr uint32_t kTouchCalibrationMagic = 0x52435054;
constexpr int kTouchCalibrationMargin = 30;

class LGFX_RCP : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Touch_XPT2046 touch_;

public:
    LGFX_RCP() {
        auto busConfig = bus_.config();
        busConfig.spi_host = static_cast<spi_host_device_t>(SPI3_HOST);
        busConfig.spi_mode = 0;
        busConfig.freq_write = 20000000;
        busConfig.freq_read = 2500000;
        busConfig.pin_sclk = pins::tft_sck;
        busConfig.pin_mosi = pins::tft_mosi;
        busConfig.pin_miso = pins::tft_miso;
        busConfig.pin_dc = pins::tft_dc;
        bus_.config(busConfig);
        panel_.setBus(&bus_);

        auto panelConfig = panel_.config();
        panelConfig.pin_cs = pins::tft_cs;
        panelConfig.pin_rst = -1;
        panelConfig.panel_width = 320;
        panelConfig.panel_height = 480;
        panelConfig.memory_width = 320;
        panelConfig.memory_height = 480;
        panelConfig.readable = false;
        panel_.config(panelConfig);

        auto touchConfig = touch_.config();
        touchConfig.spi_host = static_cast<spi_host_device_t>(SPI3_HOST);
        touchConfig.bus_shared = true;
        touchConfig.pin_cs = pins::touch_cs;
        touchConfig.pin_int = -1;
        touchConfig.freq = 2500000;
        touchConfig.x_min = 300;
        touchConfig.x_max = 3800;
        touchConfig.y_min = 200;
        touchConfig.y_max = 3700;
        touchConfig.offset_rotation = 4;
        touch_.config(touchConfig);
        panel_.setTouch(&touch_);
        setPanel(&panel_);
    }
};

enum class Language : uint8_t { Polish, English, Spanish, German };
enum class Mode : uint8_t { Offline, Online };
enum class UiFont : uint8_t { Unicode, FreeSans, FreeSerif, FreeMono };
enum class Screen : uint8_t {
    Home,
    UnknownCard,
    ActionChoice,
    PinEntry,
    EmployeeEnroll,
    Settings,
    PinChange,
    PinConfirm,
    ClockSet,
    EmployeeList,
    AddEmployeeCard,
    AttendanceConfirmation
};
enum class AttendanceAction : uint8_t { Entry, Exit, Door };

struct Employee {
    String id;
    String uid;
    String firstName;
    String lastName;
    bool active = true;
    bool relayAccess = false;
};

struct Config {
    Mode mode = Mode::Offline;
    Language language = Language::Polish;
    String ssid;
    String wifiPassword;
    String appsScriptUrl;
    String apiToken;
    String employeesSheetUrl;
    String attendanceSheetUrl;
    String logsSheetUrl;
    int timezoneOffsetMinutes = 60;
    bool cameraEnabled = false;
};

struct TouchCalibration {
    uint32_t magic = 0;
    float x[3] = {};
    float y[3] = {};
};

LGFX_RCP display;
// Arduino's HSPI maps to the ESP32-S3 SPI3 peripheral used by LovyanGFX.
SPIClass sdBus(HSPI);
MFRC522 rfid(pins::rfid_cs, pins::rfid_rst);
RTC_DS3231 rtc;
Preferences preferences;
UiFont uiFont = UiFont::Unicode;

void setUiTextSize(float size) {
    const float fontScale = uiFont == UiFont::Unicode ? 0.8f : 0.35f;
    display.setTextSize(size * fontScale);
}

void applyUiFont() {
    switch (uiFont) {
        case UiFont::FreeSans:
            display.setFont(&lgfx::fonts::FreeSans12pt7b);
            break;
        case UiFont::FreeSerif:
            display.setFont(&lgfx::fonts::FreeSerif12pt7b);
            break;
        case UiFont::FreeMono:
            display.setFont(&lgfx::fonts::FreeMono12pt7b);
            break;
        case UiFont::Unicode:
        default:
            display.setFont(&lgfx::fonts::efontJA_10);
            break;
    }
}

void drawVerticalScrollbar(int16_t x, int16_t y, int16_t height,
                           size_t totalItems, size_t visibleItems, size_t offset) {
    constexpr int16_t kScrollbarWidth = 18;
    const int16_t radius = kScrollbarWidth / 2;
    display.fillRoundRect(x, y, kScrollbarWidth, height, radius,
                          display.color565(39, 54, 82));
    if (totalItems <= visibleItems) return;
    const int16_t thumbHeight = max<int16_t>(
        36, static_cast<int16_t>((static_cast<int32_t>(height) * visibleItems) /
                                  totalItems));
    const size_t maxOffset = totalItems - visibleItems;
    const int16_t thumbY = y + static_cast<int16_t>(
        (static_cast<int32_t>(height - thumbHeight) * offset) / maxOffset);
    display.fillRoundRect(x, thumbY, kScrollbarWidth, thumbHeight, radius,
                          display.color565(129, 172, 255));
}

TouchCalibration touchCalibration;
Config config;
std::vector<Employee> employees;
Screen screen = Screen::Home;
Employee selectedEmployee;
String pinInput;
String storedPin = "0000";
String lastCardUid;
String lastMessage;
String lastHomeDate;
String lastHomeQuoteDate;
uint32_t lastCardAt = 0;
uint32_t lastClockDraw = 0;
uint32_t relayOffAt = 0;
uint32_t eventCounter = 0;
bool rtcAvailable = false;
bool sdAvailable = false;
bool rfidAvailable = false;
bool cameraAvailable = false;
esp_err_t cameraInitError = ESP_OK;
bool configurationValid = false;
bool offlineEmployeesLoaded = false;
bool onlineEmployeesLoaded = false;
bool sdFilesInitialized = false;
bool relayWasActive = false;
volatile bool relayAutoClosed = false;
String pendingPin;
String pendingEmployeeUid;
String newEmployeeId;
String newEmployeeFirstName;
String newEmployeeLastName;
String attendanceConfirmationTitle;
String attendanceConfirmationName;
bool attendanceConfirmationSuccess = false;
bool employeeEnrollmentRequested = false;
bool newEmployeeRelayAccess = false;
bool enrollmentNumericKeyboard = false;
bool enrollmentLowercase = false;
uint8_t enrollmentField = 0;
uint8_t enrollmentAccentIndex = 0;
uint32_t screenSince = 0;
uint32_t attendanceConfirmationUntil = 0;
uint32_t nextNetworkAttemptAt = 0;
uint32_t nextEmployeeRefreshAt = 0;
uint16_t employeeListPage = 0;
uint8_t settingsScroll = 0;
bool employeeManagementReturnToList = false;
esp_timer_handle_t relayTimer = nullptr;
constexpr char kGoogleRootCertificate[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
    "CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
    "MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
    "MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
    "Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
    "A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
    "27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
    "Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
    "TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
    "qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
    "szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
    "Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
    "MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
    "wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
    "aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
    "VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
    "AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
    "FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
    "C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
    "QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
    "h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
    "7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
    "ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
    "MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
    "Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
    "6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
    "0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
    "2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
    "bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
    "-----END CERTIFICATE-----\n";

uint16_t color(uint8_t red, uint8_t green, uint8_t blue) {
    return display.color565(red, green, blue);
}

const char* text(const char* polish, const char* english, const char* spanish,
                 const char* german) {
    switch (config.language) {
        case Language::English:
            return english;
        case Language::Spanish:
            return spanish;
        case Language::German:
            return german;
        case Language::Polish:
        default:
            return polish;
    }
}

const char* uiFontLabel() {
    switch (uiFont) {
        case UiFont::FreeSans:
            return "FreeSans";
        case UiFont::FreeSerif:
            return "FreeSerif";
        case UiFont::FreeMono:
            return "FreeMono";
        case UiFont::Unicode:
        default:
            return "Unicode";
    }
}

String languageLabel() {
    switch (config.language) {
        case Language::English: return "English";
        case Language::Spanish: return "Español";
        case Language::German: return "Deutsch";
        case Language::Polish:
        default: return "Polski";
    }
}

const char* modeLabel() {
    return config.mode == Mode::Offline
               ? text("Offline", "Offline", "Sin conexión", "Offline")
               : text("Online", "Online", "En línea", "Online");
}

struct MotivationQuote {
    const char* polish;
    const char* english;
    const char* spanish;
    const char* german;
};

constexpr MotivationQuote kMotivationQuotes[] = {
    {"Zacznij tam, gdzie jesteś.", "Start where you are.", "Empieza donde estás.", "Fang an, wo du bist."},
    {"Mały krok też prowadzi naprzód.", "A small step still moves you forward.", "Un paso pequeño también te acerca.", "Auch ein kleiner Schritt bringt dich weiter."},
    {"Twoja praca ma znaczenie.", "Your work makes a difference.", "Tu trabajo marca la diferencia.", "Deine Arbeit macht einen Unterschied."},
    {"Rób dziś to, co możesz.", "Do what you can today.", "Haz hoy lo que puedas.", "Tu heute, was du kannst."},
    {"Wytrwałość buduje wielkie rzeczy.", "Persistence builds great things.", "La constancia construye grandes cosas.", "Ausdauer schafft Großes."},
    {"Każdy dzień to nowa szansa.", "Every day is a new chance.", "Cada día es una nueva oportunidad.", "Jeder Tag ist eine neue Chance."},
    {"Działaj spokojnie i pewnie.", "Work calmly and with confidence.", "Actúa con calma y confianza.", "Handle ruhig und selbstbewusst."},
    {"Razem osiągamy więcej.", "Together, we achieve more.", "Juntos logramos más.", "Gemeinsam erreichen wir mehr."},
    {"Twoje zaangażowanie się liczy.", "Your commitment counts.", "Tu compromiso cuenta.", "Dein Einsatz zählt."},
    {"Dobra praca zaczyna się od Ciebie.", "Good work starts with you.", "Un buen trabajo empieza contigo.", "Gute Arbeit beginnt bei dir."},
    {"Ucz się, próbuj, rozwijaj się.", "Learn, try, and grow.", "Aprende, prueba y crece.", "Lerne, probiere aus und wachse."},
    {"Konsekwencja daje efekty.", "Consistency brings results.", "La constancia da resultados.", "Beständigkeit bringt Ergebnisse."},
    {"Masz siłę, by zrobić kolejny krok.", "You have the strength for the next step.", "Tienes fuerza para dar el siguiente paso.", "Du hast die Kraft für den nächsten Schritt."},
    {"Wspólny cel jednoczy zespół.", "A shared goal brings a team together.", "Un objetivo común une al equipo.", "Ein gemeinsames Ziel verbindet das Team."},
    {"Jakość tkwi w codziennych wyborach.", "Quality lives in everyday choices.", "La calidad está en las decisiones diarias.", "Qualität zeigt sich in täglichen Entscheidungen."},
    {"Twoja uważność zwiększa bezpieczeństwo.", "Your focus helps keep everyone safe.", "Tu atención ayuda a mantenernos seguros.", "Deine Aufmerksamkeit sorgt für mehr Sicherheit."},
    {"Dziś możesz zrobić coś dobrze.", "You can do something well today.", "Hoy puedes hacer algo bien.", "Heute kannst du etwas gut machen."},
    {"Dobra współpraca zaczyna się od szacunku.", "Good teamwork begins with respect.", "El buen trabajo en equipo empieza con respeto.", "Gute Zusammenarbeit beginnt mit Respekt."},
    {"Nie poddawaj się po pierwszej próbie.", "Don't give up after the first try.", "No te rindas tras el primer intento.", "Gib nach dem ersten Versuch nicht auf."},
    {"Postęp tworzy się krok po kroku.", "Progress is made one step at a time.", "El progreso se construye paso a paso.", "Fortschritt entsteht Schritt für Schritt."},
    {"Twoje doświadczenie pomaga innym.", "Your experience helps others.", "Tu experiencia ayuda a los demás.", "Deine Erfahrung hilft anderen."},
    {"Dobre nastawienie pomaga działać.", "A positive attitude helps you act.", "Una actitud positiva ayuda a avanzar.", "Eine positive Haltung hilft beim Anpacken."},
    {"Zadbaj o szczegóły, a efekt się obroni.", "Care for the details; results will follow.", "Cuida los detalles y llegarán los resultados.", "Achte auf Details, dann stimmt das Ergebnis."},
    {"Każde zadanie to okazja do rozwoju.", "Every task is a chance to grow.", "Cada tarea es una oportunidad para crecer.", "Jede Aufgabe bietet eine Chance zu wachsen."},
    {"Twoja rzetelność buduje zaufanie.", "Your reliability builds trust.", "Tu fiabilidad genera confianza.", "Deine Zuverlässigkeit schafft Vertrauen."},
    {"Działając razem, pokonamy trudności.", "Together, we can overcome challenges.", "Juntos podemos superar las dificultades.", "Gemeinsam meistern wir Herausforderungen."},
    {"Zacznij od tego, co najważniejsze.", "Begin with what matters most.", "Empieza por lo más importante.", "Beginne mit dem Wichtigsten."},
    {"Cierpliwość pomaga osiągać cele.", "Patience helps you reach your goals.", "La paciencia ayuda a alcanzar tus metas.", "Geduld hilft dir, deine Ziele zu erreichen."},
    {"Twoja energia inspiruje zespół.", "Your energy inspires the team.", "Tu energía inspira al equipo.", "Deine Energie inspiriert das Team."},
    {"Dobra zmiana zaczyna się od działania.", "Positive change starts with action.", "El cambio positivo empieza actuando.", "Gute Veränderung beginnt mit Handeln."},
    {"Dbaj o jakość każdego kroku.", "Bring care to every step.", "Cuida la calidad en cada paso.", "Achte bei jedem Schritt auf Qualität."},
    {"Wyzwania pokazują, ile potrafisz.", "Challenges show what you can do.", "Los retos muestran de lo que eres capaz.", "Herausforderungen zeigen, was du kannst."},
    {"Dziel się wiedzą, wzmacniaj zespół.", "Share knowledge and strengthen the team.", "Comparte lo que sabes y fortalece al equipo.", "Teile dein Wissen und stärke das Team."},
    {"Dziś jest dobry dzień na postęp.", "Today is a good day to make progress.", "Hoy es un buen día para avanzar.", "Heute ist ein guter Tag für Fortschritt."},
    {"Twoja dokładność robi różnicę.", "Your attention to detail matters.", "Tu precisión marca la diferencia.", "Deine Genauigkeit macht den Unterschied."},
    {"Odwaga rośnie z każdym działaniem.", "Courage grows with every action.", "El valor crece con cada acción.", "Mit jedem Schritt wächst der Mut."},
    {"Wspieraj innych, a zajdziemy dalej.", "Support others and we'll go further.", "Apoya a los demás y llegaremos más lejos.", "Unterstütze andere, dann kommen wir weiter."},
    {"Dobre nawyki tworzą dobre wyniki.", "Good habits create good results.", "Los buenos hábitos crean buenos resultados.", "Gute Gewohnheiten führen zu guten Ergebnissen."},
    {"Każdy wkład jest ważny.", "Every contribution matters.", "Cada aportación es importante.", "Jeder Beitrag ist wichtig."},
    {"Wykorzystaj dobrze ten dzień.", "Make good use of today.", "Aprovecha bien el día de hoy.", "Nutze den heutigen Tag gut."},
    {"Szukaj rozwiązań, nie przeszkód.", "Look for solutions, not obstacles.", "Busca soluciones, no obstáculos.", "Suche Lösungen statt Hindernisse."},
    {"Twoja praca tworzy wspólny sukces.", "Your work builds shared success.", "Tu trabajo crea un éxito compartido.", "Deine Arbeit schafft gemeinsamen Erfolg."},
    {"Spokój pomaga podejmować dobre decyzje.", "Calm helps you make good decisions.", "La calma ayuda a tomar buenas decisiones.", "Ruhe hilft, gute Entscheidungen zu treffen."},
    {"Warto robić rzeczy porządnie.", "Doing things well is worth it.", "Vale la pena hacer bien las cosas.", "Es lohnt sich, die Dinge gut zu machen."},
    {"Z każdym dniem umiesz więcej.", "You know more with every passing day.", "Cada día sabes hacer un poco más.", "Mit jedem Tag kannst du mehr."},
    {"Wspólny wysiłek przynosi efekty.", "A shared effort brings results.", "El esfuerzo compartido da frutos.", "Gemeinsamer Einsatz zahlt sich aus."},
    {"Działaj z troską i odpowiedzialnością.", "Act with care and responsibility.", "Actúa con cuidado y responsabilidad.", "Handle sorgfältig und verantwortungsvoll."},
    {"Małe usprawnienia robią wielką różnicę.", "Small improvements make a big difference.", "Las pequeñas mejoras marcan una gran diferencia.", "Kleine Verbesserungen bewirken viel."},
    {"Masz wpływ na jakość swojej pracy.", "You can shape the quality of your work.", "Puedes influir en la calidad de tu trabajo.", "Du beeinflusst die Qualität deiner Arbeit."},
    {"Zakończ dzień z poczuciem dobrze wykonanej pracy.", "End the day proud of work well done.", "Termina el día con el orgullo del trabajo bien hecho.", "Beende den Tag mit dem guten Gefühl, etwas geschafft zu haben."}
};

constexpr size_t kMotivationQuoteCount =
    sizeof(kMotivationQuotes) / sizeof(kMotivationQuotes[0]);

uint8_t dailyMotivationIndex() {
    static uint8_t fallbackIndex = static_cast<uint8_t>(esp_random() % kMotivationQuoteCount);
    if (!rtcAvailable) return fallbackIndex;

    const DateTime now = rtc.now();
    const uint32_t dateKey = static_cast<uint32_t>(now.year()) * 10000U +
                             static_cast<uint32_t>(now.month()) * 100U + now.day();
    const uint32_t savedDate = preferences.getUInt("quote_day", 0);
    const uint8_t savedIndex = preferences.getUChar("quote_idx", 0);
    if (savedDate == dateKey && savedIndex < kMotivationQuoteCount) {
        return savedIndex;
    }

    uint8_t index = static_cast<uint8_t>(esp_random() % kMotivationQuoteCount);
    if (savedDate != 0 && savedIndex < kMotivationQuoteCount &&
        kMotivationQuoteCount > 1 && index == savedIndex) {
        index = static_cast<uint8_t>((index + 1) % kMotivationQuoteCount);
    }
    if (preferences.putUChar("quote_idx", index) == 0 ||
        preferences.putUInt("quote_day", dateKey) == 0) {
        Serial.println("Could not persist the daily motivation quote selection");
    }
    return index;
}

const char* dailyMotivationQuote() {
    const MotivationQuote& quote = kMotivationQuotes[dailyMotivationIndex()];
    switch (config.language) {
        case Language::English: return quote.english;
        case Language::Spanish: return quote.spanish;
        case Language::German: return quote.german;
        case Language::Polish:
        default: return quote.polish;
    }
}

std::vector<String> parseCsvRow(const String& line) {
    std::vector<String> fields;
    String field;
    bool quoted = false;
    for (size_t index = 0; index < line.length(); ++index) {
        const char value = line[index];
        if (value == '"') {
            if (quoted && index + 1 < line.length() && line[index + 1] == '"') {
                field += '"';
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (value == ',' && !quoted) {
            fields.push_back(field);
            field = "";
        } else {
            field += value;
        }
    }
    fields.push_back(field);
    return fields;
}

bool readCsvRecord(File& file, String& record) {
    record = "";
    bool inQuotes = false;
    bool readAny = false;
    while (file.available()) {
        const char value = static_cast<char>(file.read());
        readAny = true;
        if (value == '"') {
            if (inQuotes && file.peek() == '"') {
                record += "\"\"";
                file.read();
            } else {
                inQuotes = !inQuotes;
                record += value;
            }
        } else if (value == '\n' && !inQuotes) {
            return true;
        } else if (value != '\r') {
            record += value;
        }
    }
    return readAny;
}

String csvEscape(const String& value) {
    String output = "\"";
    for (size_t index = 0; index < value.length(); ++index) {
        if (value[index] == '"') output += '"';
        output += value[index];
    }
    output += '"';
    return output;
}

String currentTimestamp();
String capturePhoto();
void drawHome();
void drawAttendanceConfirmation();
size_t activeEmployeeCount();

bool ensureCsvFile(const char* path, const char* header) {
    if (!sdAvailable) return false;
    if (SD.exists(path)) {
        File existing = SD.open(path, FILE_READ);
        if (!existing) {
            Serial.printf("Could not inspect %s\n", path);
            return false;
        }
        const bool hasContent = existing.size() > 0;
        existing.close();
        if (hasContent) return true;
    }
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("Could not create %s\n", path);
        return false;
    }
    const size_t written = file.println(header);
    file.flush();
    file.close();
    if (written == 0) {
        Serial.printf("Could not write header to %s\n", path);
        return false;
    }
    Serial.printf("Initialized empty %s\n", path);
    return true;
}

bool postToAppsScript(JsonDocument& request, JsonDocument* response = nullptr) {
    if (config.mode != Mode::Online || WiFi.status() != WL_CONNECTED ||
        config.appsScriptUrl.isEmpty() || config.apiToken.isEmpty()) {
        return false;
    }
    request["token"] = config.apiToken;
    String body;
    serializeJson(request, body);

    WiFiClientSecure secureClient;
    secureClient.setCACert(kGoogleRootCertificate);
    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(secureClient, config.appsScriptUrl)) return false;
    http.addHeader("Content-Type", "application/json");
    const int status = http.POST(body);
    if (status < 200 || status >= 300) {
        Serial.printf("Apps Script HTTP error: %d\n", status);
        http.end();
        return false;
    }
    const String responseBody = http.getString();
    http.end();
    JsonDocument parsed;
    if (deserializeJson(parsed, responseBody) != DeserializationError::Ok ||
        !(parsed["ok"] | false)) {
        Serial.println("Apps Script returned an invalid or unsuccessful response");
        return false;
    }
    if (response != nullptr) *response = parsed;
    return true;
}

void addLog(const char* level, const char* code, const String& message) {
    const String eventId = String(millis()) + "-" + String(++eventCounter);
    const String timestamp = currentTimestamp();
    bool localLogWritten = false;
    if (!sdAvailable) {
        Serial.printf("Log not saved locally (SD unavailable): %s %s\n",
                      code, message.c_str());
    } else {
        File file = SD.open(kLogsPath, FILE_APPEND);
        if (!file) {
            Serial.printf("Could not append log to SD: %s\n", code);
        } else {
            if (file.size() == 0) {
                file.println("event_id,timestamp,level,code,message");
            }
            const String csvRow =
                csvEscape(eventId) + "," + csvEscape(timestamp) + "," +
                csvEscape(level) + "," + csvEscape(code) + "," +
                csvEscape(message) + "\n";
            const size_t written = file.print(csvRow);
            file.flush();
            file.close();
            localLogWritten = written == csvRow.length();
            if (!localLogWritten) {
                Serial.printf("Incomplete SD log write: %s\n", code);
            }
        }
    }

    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "log";
        JsonObject row = request["row"].to<JsonObject>();
        row["event_id"] = eventId;
        row["timestamp"] = timestamp;
        row["level"] = level;
        row["code"] = code;
        row["message"] = message;
        if (!postToAppsScript(request)) {
            Serial.printf("Online log upload failed%s: %s\n",
                          localLogWritten ? "; retained on SD" : "", code);
        }
    }
}

bool loadConfig() {
    if (!sdAvailable) return false;
    File file = SD.open(kConfigPath, FILE_READ);
    if (!file) return false;

    JsonDocument document;
    const DeserializationError error = deserializeJson(document, file);
    file.close();
    if (error) {
        Serial.printf("Invalid config.json: %s\n", error.c_str());
        return false;
    }

    if ((document["schema_version"] | 0) != 1) {
        Serial.println("Unsupported config.json schema_version");
        return false;
    }
    const char* mode = document["mode"] | "";
    const char* language = document["language"] | "";
    if (strcmp(mode, "offline") == 0) {
        config.mode = Mode::Offline;
    } else if (strcmp(mode, "online") == 0) {
        config.mode = Mode::Online;
    } else {
        Serial.println("config.json must explicitly select offline or online mode");
        return false;
    }
    config.ssid = document["wifi_ssid"] | "";
    config.wifiPassword = document["wifi_password"] | "";
    config.appsScriptUrl = document["apps_script_url"] | "";
    config.apiToken = document["api_token"] | "";
    config.employeesSheetUrl = document["employees_sheet_url"] | "";
    config.attendanceSheetUrl = document["attendance_sheet_url"] | "";
    config.logsSheetUrl = document["logs_sheet_url"] | "";
    config.timezoneOffsetMinutes = document["timezone_offset_minutes"] | 60;
    config.cameraEnabled = document["camera_enabled"] | false;

    if (strcmp(language, "pl") == 0) config.language = Language::Polish;
    else if (strcmp(language, "en") == 0) config.language = Language::English;
    else if (strcmp(language, "es") == 0) config.language = Language::Spanish;
    else if (strcmp(language, "de") == 0) config.language = Language::German;
    else {
        Serial.println("config.json has an unsupported language");
        return false;
    }
    if (config.mode == Mode::Online) {
        if (config.ssid.isEmpty() ||
            !config.appsScriptUrl.startsWith("https://script.google.com/macros/s/") ||
            !config.appsScriptUrl.endsWith("/exec") || config.apiToken.length() < 32 ||
            !config.employeesSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
            !config.attendanceSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
            !config.logsSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/")) {
            Serial.println("Online config is incomplete; refusing offline fallback");
            return false;
        }
    }
    configurationValid = true;
    return true;
}

bool saveConfig() {
    if (!sdAvailable) return false;
    if (config.mode == Mode::Online &&
        (config.ssid.isEmpty() ||
         !config.appsScriptUrl.startsWith("https://script.google.com/macros/s/") ||
         !config.appsScriptUrl.endsWith("/exec") || config.apiToken.length() < 32 ||
         !config.employeesSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
         !config.attendanceSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/") ||
         !config.logsSheetUrl.startsWith("https://docs.google.com/spreadsheets/d/"))) {
        lastMessage = text("Uzupełnij Wi-Fi, API i trzy arkusze",
                           "Configure Wi-Fi, API and all three sheets",
                           "Configure Wi-Fi, API y las tres hojas",
                           "WLAN, API und alle drei Tabellen konfigurieren");
        return false;
    }
    JsonDocument document;
    document["schema_version"] = 1;
    document["mode"] = config.mode == Mode::Offline ? "offline" : "online";
    switch (config.language) {
        case Language::English: document["language"] = "en"; break;
        case Language::Spanish: document["language"] = "es"; break;
        case Language::German: document["language"] = "de"; break;
        case Language::Polish:
        default: document["language"] = "pl"; break;
    }
    document["wifi_ssid"] = config.ssid;
    document["wifi_password"] = config.wifiPassword;
    document["timezone_offset_minutes"] = config.timezoneOffsetMinutes;
    document["apps_script_url"] = config.appsScriptUrl;
    document["api_token"] = config.apiToken;
    document["employees_sheet_url"] = config.employeesSheetUrl;
    document["attendance_sheet_url"] = config.attendanceSheetUrl;
    document["logs_sheet_url"] = config.logsSheetUrl;
    document["camera_enabled"] = config.cameraEnabled;

    constexpr char temporaryPath[] = "/config.json.tmp";
    constexpr char backupPath[] = "/config.json.bak";
    File file = SD.open(temporaryPath, FILE_WRITE);
    if (!file) return false;
    const size_t written = serializeJson(document, file);
    file.flush();
    file.close();
    if (written == 0) {
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    const bool hadOriginal = SD.exists(kConfigPath);
    if (hadOriginal && !SD.rename(kConfigPath, backupPath)) {
        SD.remove(temporaryPath);
        return false;
    }
    if (!SD.rename(temporaryPath, kConfigPath)) {
        if (hadOriginal) SD.rename(backupPath, kConfigPath);
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    configurationValid = true;
    return true;
}

bool loadEmployeesFromCsv() {
    employees.clear();
    offlineEmployeesLoaded = false;
    if (!sdAvailable) return false;
    File file = SD.open(kEmployeesPath, FILE_READ);
    if (!file) return false;
    String line;
    if (!readCsvRecord(file, line)) {
        file.close();
        return false;
    }
    if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
    if (line != "employee_id,card_uid,first_name,last_name,active,relay_access") {
        file.close();
        Serial.println("employees.csv has an unsupported header");
        return false;
    }
    while (employees.size() < kMaximumEmployees && readCsvRecord(file, line)) {
        if (line.isEmpty()) continue;
        const auto fields = parseCsvRow(line);
        if (fields.size() != 6) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains a row with the wrong number of columns");
            return false;
        }
        Employee employee;
        employee.id = fields[0];
        employee.uid = fields[1];
        employee.uid.toUpperCase();
        employee.firstName = fields[2];
        employee.lastName = fields[3];
        String active = fields[4];
        String relayAccess = fields[5];
        active.toLowerCase();
        relayAccess.toLowerCase();
        if ((active != "true" && active != "false" && active != "1" && active != "0") ||
            (relayAccess != "true" && relayAccess != "false" &&
             relayAccess != "1" && relayAccess != "0")) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains an invalid boolean value");
            return false;
        }
        employee.active = active == "true" || active == "1";
        employee.relayAccess = relayAccess == "true" || relayAccess == "1";
        bool duplicate = employee.id.isEmpty() || employee.uid.isEmpty();
        for (const auto& existing : employees) {
            if (existing.id == employee.id || existing.uid == employee.uid) duplicate = true;
        }
        if (duplicate) {
            file.close();
            employees.clear();
            Serial.println("employees.csv contains empty or duplicate employee/card IDs");
            return false;
        }
        employees.push_back(employee);
    }
    file.close();
    offlineEmployeesLoaded = true;
    return true;
}

bool persistEmployeesOffline(const std::vector<Employee>& rows) {
    constexpr char temporaryPath[] = "/employees.csv.tmp";
    constexpr char backupPath[] = "/employees.csv.bak";
    SD.remove(temporaryPath);
    SD.remove(backupPath);
    File file = SD.open(temporaryPath, FILE_WRITE);
    if (!file) return false;

    bool writeOk =
        file.println("employee_id,card_uid,first_name,last_name,active,relay_access") > 0;
    for (const auto& employee : rows) {
        if (!writeOk) break;
        const String csvRow = csvEscape(employee.id) + "," + csvEscape(employee.uid) + "," +
                              csvEscape(employee.firstName) + "," +
                              csvEscape(employee.lastName) + "," +
                              (employee.active ? "true" : "false") + "," +
                              (employee.relayAccess ? "true" : "false") + "\n";
        writeOk = file.print(csvRow) == csvRow.length();
    }
    file.flush();
    file.close();
    if (!writeOk) {
        SD.remove(temporaryPath);
        return false;
    }

    const bool hadOriginal = SD.exists(kEmployeesPath);
    if (hadOriginal && !SD.rename(kEmployeesPath, backupPath)) {
        SD.remove(temporaryPath);
        return false;
    }
    if (!SD.rename(temporaryPath, kEmployeesPath)) {
        if (hadOriginal && !SD.rename(backupPath, kEmployeesPath)) {
            Serial.println("CRITICAL: Could not restore employees.csv backup");
        }
        SD.remove(temporaryPath);
        return false;
    }
    SD.remove(backupPath);
    return true;
}

bool updateEmployeeRecord(size_t index, bool active, bool relayAccess) {
    if (index >= employees.size()) return false;
    std::vector<Employee> updated = employees;
    updated[index].active = active;
    updated[index].relayAccess = relayAccess;

    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "updateEmployee";
        JsonObject row = request["row"].to<JsonObject>();
        row["employee_id"] = updated[index].id;
        row["card_uid"] = updated[index].uid;
        row["active"] = active;
        row["relay_access"] = relayAccess;
        if (!postToAppsScript(request)) {
            lastMessage = text("Błąd aktualizacji w arkuszu",
                               "Could not update employee sheet",
                               "No se pudo actualizar la hoja",
                               "Mitarbeiterblatt konnte nicht aktualisiert werden");
            return false;
        }
    } else if (!sdAvailable || !offlineEmployeesLoaded ||
               !persistEmployeesOffline(updated)) {
        lastMessage = text("Nie można zapisać employees.csv",
                           "Could not save employees.csv",
                           "No se pudo guardar employees.csv",
                           "employees.csv konnte nicht gespeichert werden");
        return false;
    }

    employees = std::move(updated);
    addLog("INFO", active ? "EMPLOYEE_UPDATED" : "EMPLOYEE_REMOVED",
           employees[index].id);
    lastMessage = active
                      ? (relayAccess
                             ? text("Nadano dostęp do przekaźnika",
                                    "Relay access enabled",
                                    "Acceso al relé habilitado",
                                    "Relaiszugriff aktiviert")
                             : text("Zmieniono uprawnienia",
                                    "Permissions updated",
                                    "Permisos actualizados",
                                    "Berechtigungen aktualisiert"))
                      : text("Pracownik usunięty",
                             "Employee removed", "Empleado eliminado",
                             "Mitarbeiter entfernt");
    return true;
}

bool loadEmployeesFromOnlineSheet() {
    employees.clear();
    onlineEmployeesLoaded = false;
    JsonDocument request;
    request["action"] = "employees";
    JsonDocument response;
    if (!postToAppsScript(request, &response)) return false;
    JsonArrayConst rows = response["rows"].as<JsonArrayConst>();
    for (JsonObjectConst row : rows) {
        if (employees.size() >= kMaximumEmployees) break;
        Employee employee;
        employee.id = row["employee_id"] | "";
        employee.uid = row["card_uid"] | "";
        employee.uid.toUpperCase();
        employee.firstName = row["first_name"] | "";
        employee.lastName = row["last_name"] | "";
        String active = row["active"] | "true";
        String relayAccess = row["relay_access"] | "false";
        active.toLowerCase();
        relayAccess.toLowerCase();
        if ((active != "true" && active != "false" && active != "1" && active != "0") ||
            (relayAccess != "true" && relayAccess != "false" &&
             relayAccess != "1" && relayAccess != "0")) {
            employees.clear();
            Serial.println("Online employee sheet contains an invalid boolean value");
            return false;
        }
        employee.active = active == "true" || active == "1";
        employee.relayAccess = relayAccess == "true" || relayAccess == "1";
        bool duplicate = employee.id.isEmpty() || employee.uid.isEmpty();
        for (const auto& existing : employees) {
            if (existing.id == employee.id || existing.uid == employee.uid) duplicate = true;
        }
        if (duplicate) {
            employees.clear();
            Serial.println("Online employee sheet contains empty or duplicate identifiers");
            return false;
        }
        employees.push_back(employee);
    }
    onlineEmployeesLoaded = true;
    return true;
}

String currentTimestamp() {
    if (!rtcAvailable) return "";
    const DateTime now = rtc.now();
    char buffer[26];
    const int offset = config.timezoneOffsetMinutes;
    const char sign = offset < 0 ? '-' : '+';
    const int absoluteOffset = abs(offset);
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second(),
             sign, absoluteOffset / 60, absoluteOffset % 60);
    return String(buffer);
}

bool previousAttendance(const String& employeeId, String& previous) {
    previous = "";
    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "lastAttendance";
        request["employee_id"] = employeeId;
        JsonDocument response;
        if (!postToAppsScript(request, &response)) return false;
        previous = response["last_event_type"] | "";
        return true;
    }
    if (!sdAvailable) return false;
    File file = SD.open(kAttendancePath, FILE_READ);
    if (!file) return true;
    String line;
    readCsvRecord(file, line);
    while (readCsvRecord(file, line)) {
        const auto fields = parseCsvRow(line);
        if (fields.size() >= 5 && fields[2] == employeeId &&
            (fields[4] == "ENTRY" || fields[4] == "EXIT")) {
            previous = fields[4];
        }
    }
    file.close();
    return true;
}

bool appendAttendance(const Employee& employee, AttendanceAction action,
                      const String& photoPath, bool automatic, String& recordedType) {
    const String eventId = String(millis()) + "-" + String(esp_random(), HEX) +
                           "-" + String(++eventCounter);
    recordedType = action == AttendanceAction::Entry ? "ENTRY" : "EXIT";
    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = automatic ? "punch" : "attendance";
        JsonObject row = request["row"].to<JsonObject>();
        row["event_id"] = eventId;
        row["timestamp"] = currentTimestamp();
        row["employee_id"] = employee.id;
        row["card_uid"] = employee.uid;
        if (!automatic) row["event_type"] = recordedType;
        row["photo_file"] = photoPath;
        row["source"] = "online";
        row["notes"] = "";
        JsonDocument response;
        if (!postToAppsScript(request, &response)) {
            lastMessage = text("Błąd połączenia — brak zapisu", "Connection failed — not saved",
                              "Error de conexión — no guardado",
                              "Verbindung fehlgeschlagen — nicht gespeichert");
            Serial.printf("Attendance API failed; event not stored locally: %s\n",
                          employee.id.c_str());
            return false;
        }
        if (automatic) {
            recordedType = response["event_type"] | "";
            if (recordedType != "ENTRY" && recordedType != "EXIT") {
                lastMessage = text("Błędna odpowiedź API", "Invalid API response",
                                   "Respuesta API inválida", "Ungültige API-Antwort");
                return false;
            }
        }
    } else {
        if (!sdAvailable) return false;
        File file = SD.open(kAttendancePath, FILE_APPEND);
        if (!file) {
            addLog("ERROR", "ATTENDANCE_WRITE_FAILED", "Nie można dopisać attendance.csv");
            return false;
        }
        if (file.size() == 0) {
            file.println("event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes");
        }
        file.printf("%s,%s,%s,%s,%s,%s,%s,%s\n",
                    csvEscape(eventId).c_str(), csvEscape(currentTimestamp()).c_str(),
                    csvEscape(employee.id).c_str(), csvEscape(employee.uid).c_str(),
                    csvEscape(recordedType).c_str(), csvEscape(photoPath).c_str(),
                    csvEscape("offline").c_str(), csvEscape("").c_str());
        file.close();
    }
    return true;
}

void processAttendance(AttendanceAction action, bool automatic = false) {
    lastMessage = "";
    const String photo = capturePhoto();
    String recordedType;
    if (!appendAttendance(selectedEmployee, action, photo, automatic, recordedType)) {
        attendanceConfirmationSuccess = false;
        if (lastMessage.isEmpty()) {
            lastMessage = text("Błąd zapisu", "Write failed", "Error al guardar",
                               "Schreibfehler");
        }
        attendanceConfirmationTitle = lastMessage;
        attendanceConfirmationName = "";
    } else {
        attendanceConfirmationSuccess = true;
        attendanceConfirmationTitle =
            recordedType == "ENTRY"
                ? text("Dzień dobry", "Good morning", "Buenos días", "Guten Morgen")
                : text("Do widzenia", "Goodbye", "Hasta luego", "Auf Wiedersehen");
        attendanceConfirmationName =
            selectedEmployee.firstName + " " + selectedEmployee.lastName;
        lastMessage = "";
    }
    screenSince = millis();
    attendanceConfirmationUntil = screenSince + kAttendanceConfirmationMs;
    screen = Screen::AttendanceConfirmation;
    drawAttendanceConfirmation();
}

String capturePhoto() {
    if (!config.cameraEnabled) return "";
    if (!cameraAvailable || !sdAvailable) {
        addLog("WARN", "CAMERA_UNAVAILABLE", "Aparat niedostępny");
        return "";
    }
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
        addLog("ERROR", "CAMERA_CAPTURE_FAILED", "Nie udało się wykonać zdjęcia");
        return "";
    }
    if (!SD.exists("/photos")) SD.mkdir("/photos");
    const String path = String("/photos/") + String(millis()) + ".jpg";
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        esp_camera_fb_return(frame);
        addLog("ERROR", "PHOTO_WRITE_FAILED", "Nie można zapisać zdjęcia");
        return "";
    }
    const size_t expected = frame->len;
    const size_t written = file.write(frame->buf, expected);
    file.close();
    esp_camera_fb_return(frame);
    if (written != expected) {
        addLog("ERROR", "PHOTO_WRITE_FAILED", "Zapis zdjęcia zwrócił zero bajtów");
        SD.remove(path);
        return "";
    }
    return path;
}

bool initCamera() {
    camera_config_t cameraConfig = {};
    cameraConfig.ledc_channel = LEDC_CHANNEL_0;
    cameraConfig.ledc_timer = LEDC_TIMER_0;
    cameraConfig.pin_d0 = pins::camera::y2;
    cameraConfig.pin_d1 = pins::camera::y3;
    cameraConfig.pin_d2 = pins::camera::y4;
    cameraConfig.pin_d3 = pins::camera::y5;
    cameraConfig.pin_d4 = pins::camera::y6;
    cameraConfig.pin_d5 = pins::camera::y7;
    cameraConfig.pin_d6 = pins::camera::y8;
    cameraConfig.pin_d7 = pins::camera::y9;
    cameraConfig.pin_xclk = pins::camera::xclk;
    cameraConfig.pin_pclk = pins::camera::pclk;
    cameraConfig.pin_vsync = pins::camera::vsync;
    cameraConfig.pin_href = pins::camera::href;
    cameraConfig.pin_sccb_sda = pins::camera::siod;
    cameraConfig.pin_sccb_scl = pins::camera::sioc;
    cameraConfig.pin_pwdn = pins::camera::pwdn;
    cameraConfig.pin_reset = pins::camera::reset;
    cameraConfig.xclk_freq_hz = 20000000;
    cameraConfig.pixel_format = PIXFORMAT_JPEG;
    cameraConfig.frame_size = FRAMESIZE_VGA;
    cameraConfig.jpeg_quality = 12;
    cameraConfig.fb_count = 1;
    cameraConfig.fb_location = CAMERA_FB_IN_PSRAM;
    cameraConfig.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&cameraConfig);
    if (result != ESP_OK) {
        cameraInitError = result;
        Serial.printf("Camera init failed: 0x%x\n", result);
        return false;
    }
    cameraInitError = ESP_OK;
    return true;
}

void relayTimerCallback(void*) {
    gpio_set_level(static_cast<gpio_num_t>(pins::relay), 1);
    relayAutoClosed = true;
}

void setRelay(bool enabled) {
    if (enabled) {
        digitalWrite(pins::relay, LOW);
        relayWasActive = true;
        relayOffAt = millis() + kRelayPulseMs;
        if (relayTimer != nullptr) {
            if (esp_timer_is_active(relayTimer)) esp_timer_stop(relayTimer);
            if (esp_timer_start_once(relayTimer, kRelayPulseMs * 1000) == ESP_OK) {
                relayOffAt = 0;
            }
        }
        return;
    }
    digitalWrite(pins::relay, HIGH);
    if (relayTimer != nullptr && esp_timer_is_active(relayTimer)) {
        esp_timer_stop(relayTimer);
    }
    relayOffAt = 0;
    if (relayWasActive) {
        relayWasActive = false;
        addLog("INFO", "RELAY_UNLOCK_PULSE_COMPLETED", "");
    }
}

void drawButton(int16_t x, int16_t y, int16_t width, int16_t height,
                const String& label, uint16_t fill) {
    display.fillRoundRect(x, y, width, height, 12, fill);
    display.drawRoundRect(x, y, width, height, 12, color(210, 222, 241));
    display.setTextColor(color(250, 252, 255), fill);
    setUiTextSize(2.5f);
    display.drawCenterString(label, x + width / 2, y + height / 2 - 8);
}

void drawHomeQuoteArea() {
    const uint16_t panelColor = color(22, 32, 54);
    display.fillRect(28, 207, 424, 62, panelColor);
    if (lastMessage.isEmpty()) {
        const char* quote = dailyMotivationQuote();
        const size_t quoteLength = strlen(quote);
        const float quoteSize = quoteLength > 72 ? 0.9f
                                : quoteLength > 54 ? 1.05f
                                : quoteLength > 38 ? 1.2f : 1.4f;
        display.setTextColor(color(129, 172, 255), panelColor);
        setUiTextSize(quoteSize);
        display.drawCenterString(quote, 240, 209);
    }

    const char* prompt = lastMessage.isEmpty()
                             ? text("Zbliż kartę pracownika", "Tap employee card",
                                    "Acerque la tarjeta", "Karte vorhalten")
                             : lastMessage.c_str();
    display.setTextColor(lastMessage.isEmpty() ? color(174, 190, 215)
                                                : color(255, 191, 87),
                         panelColor);
    setUiTextSize(strlen(prompt) > 48 ? 0.9f : 1.25f);
    display.drawCenterString(prompt, 240, 240);
    if (rtcAvailable) {
        const DateTime now = rtc.now();
        char dateKey[11];
        snprintf(dateKey, sizeof(dateKey), "%04d-%02d-%02d",
                 now.year(), now.month(), now.day());
        lastHomeQuoteDate = dateKey;
    }
}

void drawHome() {
    display.fillScreen(color(10, 17, 33));
    display.fillRoundRect(16, 16, 448, 288, 20, color(22, 32, 54));
    display.setTextColor(color(129, 172, 255));
    setUiTextSize(1.5f);
    display.drawString("RCP2027 PROMIR-BUD PASSION LINE", 28, 30);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1.5f);
    display.drawRightString(config.mode == Mode::Offline ? "OFFLINE" : "ONLINE",
                            442, 48);
    display.setTextColor(color(248, 250, 255));
    setUiTextSize(7.5f);
    if (rtcAvailable) {
        const DateTime now = rtc.now();
        char clockText[10];
        snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
                 now.hour(), now.minute(), now.second());
        display.drawCenterString(clockText, 240, 112);
        char dateText[20];
        snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d",
                 now.year(), now.month(), now.day());
        lastHomeDate = dateText;
        setUiTextSize(3.5f);
        display.setTextColor(color(174, 190, 215));
        display.drawCenterString(dateText, 240, 182);
    } else {
        setUiTextSize(2.5f);
        display.setTextColor(color(255, 191, 87));
        display.drawCenterString(text("Brak zegara RTC", "RTC unavailable",
                                      "RTC no disponible", "RTC nicht verfügbar"),
                                 240, 130);
    }
    drawHomeQuoteArea();
    setUiTextSize(1.6f);
    display.setTextColor(color(205, 216, 232));
    display.drawString(configurationValid ? (sdAvailable ? "SD OK" : "SD ERROR")
                                          : "CONFIG ERROR",
                       36, 278);
    display.drawString(rfidAvailable ? "RFID OK" : "RFID ERROR", 145, 278);
    display.drawString(config.mode == Mode::Online
                           ? (WiFi.status() == WL_CONNECTED ? "WIFI OK" : "WIFI ERROR")
                           : "OFFLINE",
                       248, 278);
    display.drawRightString(text("USTAWIENIA", "SETTINGS", "AJUSTES", "EINSTELLUNGEN"),
                            442, 274);
}

void drawAttendanceConfirmation() {
    display.fillScreen(color(10, 17, 33));
    display.fillRoundRect(20, 28, 440, 264, 20, color(22, 32, 54));
    const uint16_t accent = attendanceConfirmationSuccess
                                ? color(113, 230, 173)
                                : color(255, 191, 87);
    display.setTextColor(accent);
    setUiTextSize(2.8f);
    display.drawCenterString(attendanceConfirmationTitle, 240, 104);

    if (attendanceConfirmationSuccess) {
        display.setTextColor(color(248, 250, 255));
        const size_t nameLength = attendanceConfirmationName.length();
        setUiTextSize(nameLength > 24 ? 1.8f : nameLength > 16 ? 2.2f : 2.8f);
        display.drawCenterString(attendanceConfirmationName, 240, 164);
    }
}

void drawUnknownCard() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(255, 191, 87));
    setUiTextSize(3);
    display.drawCenterString(text("Błędny kod", "Unknown card", "Código incorrecto",
                                  "Unbekannte Karte"),
                             240, 50);
    display.setTextColor(color(221, 230, 246));
    setUiTextSize(2);
    display.drawCenterString(pendingEmployeeUid, 240, 105);
    display.drawCenterString(text("Czy chcesz dodać nowy kod?",
                                  "Would you like to add this card?",
                                  "¿Desea añadir esta tarjeta?",
                                  "Möchten Sie diese Karte hinzufügen?"),
                             240, 150);
    drawButton(55, 215, 170, 62,
               text("Tak", "Yes", "Sí", "Ja"), color(25, 139, 103));
    drawButton(255, 215, 170, 62,
               text("Nie", "No", "No", "Nein"), color(103, 47, 58));
}

String* enrollmentFieldValue(uint8_t field) {
    if (field == 0) return &newEmployeeId;
    if (field == 1) return &newEmployeeFirstName;
    return &newEmployeeLastName;
}

const char* enrollmentAccentCharacter() {
    static const char* polishLower[] = {"ą", "ę", "ć", "ł", "ń", "ó", "ś", "ź", "ż"};
    static const char* polishUpper[] = {"Ą", "Ę", "Ć", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż"};
    static const char* spanishLower[] = {"á", "é", "í", "ñ", "ó", "ú", "ü"};
    static const char* spanishUpper[] = {"Á", "É", "Í", "Ñ", "Ó", "Ú", "Ü"};
    static const char* germanLower[] = {"ä", "ö", "ü", "ß"};
    static const char* germanUpper[] = {"Ä", "Ö", "Ü", "ẞ"};
    if (config.language == Language::Polish) {
        return enrollmentLowercase ? polishLower[enrollmentAccentIndex % 9]
                                   : polishUpper[enrollmentAccentIndex % 9];
    }
    if (config.language == Language::Spanish) {
        return enrollmentLowercase ? spanishLower[enrollmentAccentIndex % 7]
                                   : spanishUpper[enrollmentAccentIndex % 7];
    }
    if (config.language == Language::German) {
        return enrollmentLowercase ? germanLower[enrollmentAccentIndex % 4]
                                   : germanUpper[enrollmentAccentIndex % 4];
    }
    return enrollmentAccentIndex % 2 == 0 ? "-" : "_";
}

void removeLastInputCharacter(String& value) {
    if (value.isEmpty()) return;
    size_t index = value.length() - 1;
    while (index > 0 &&
           (static_cast<uint8_t>(value[index]) & 0xC0) == 0x80) {
        --index;
    }
    value.remove(index);
}

void drawEmployeeEnrollment() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(text("Nowy pracownik", "New employee",
                                  "Nuevo empleado", "Neuer Mitarbeiter"),
                             240, 7);
    setUiTextSize(1);
    display.setTextColor(lastMessage.isEmpty() ? color(174, 190, 215)
                                               : color(255, 150, 150));
    display.drawCenterString(
        lastMessage.isEmpty() ? "UID: " + pendingEmployeeUid : lastMessage, 240, 32);

    const char* labels[] = {
        text("ID", "ID", "ID", "ID"),
        text("Imię", "First name", "Nombre", "Vorname"),
        text("Nazwisko", "Last name", "Apellido", "Nachname")
    };
    for (uint8_t field = 0; field < 3; ++field) {
        const int y = 48 + field * 32;
        const uint16_t border = field == enrollmentField
                                    ? color(129, 172, 255)
                                    : color(75, 92, 121);
        display.fillRoundRect(20, y, 440, 32, 6, color(22, 32, 54));
        display.drawRoundRect(20, y, 440, 32, 6, border);
        display.setTextColor(color(174, 190, 215));
        setUiTextSize(2);
        display.drawString(String(labels[field]) + ":", 29, y + 7);
        display.setTextColor(color(250, 252, 255));
        display.drawString(*enrollmentFieldValue(field), 115, y + 7);
    }

    const String relayLabel =
        String(text("Dostęp do przekaźnika: ", "Relay access: ",
                    "Acceso al relé: ", "Relaiszugriff: ")) +
        (newEmployeeRelayAccess ? text("TAK", "YES", "SÍ", "JA")
                                : text("NIE", "NO", "NO", "NEIN"));
    drawButton(20, 150, 440, 25, relayLabel,
               newEmployeeRelayAccess ? color(25, 116, 88) : color(39, 54, 82));

    const char* alphaRows[] = {
        "QWERTYUIOP",
        "ASDFGHJKL",
        "ZXCVBNM"
    };
    const int rowStarts[] = {20, 42, 86};
    const int rowLengths[] = {10, 9, 7};
    const char* numberRows[] = {
        "1234567890",
        "0987654321",
        ".,-_@"
    };
    const int numberLengths[] = {10, 10, 5};
    for (int row = 0; row < 3; ++row) {
        const char* keys = enrollmentNumericKeyboard ? numberRows[row] : alphaRows[row];
        const int count = enrollmentNumericKeyboard ? numberLengths[row] : rowLengths[row];
        const int start = enrollmentNumericKeyboard ? 20 : rowStarts[row];
        for (int index = 0; index < count; ++index) {
            String label = String(keys[index]);
            if (!enrollmentNumericKeyboard && enrollmentLowercase) {
                label.toLowerCase();
            }
            const int x = start + index * 44;
            drawButton(x, 179 + row * 33, 40, 29, label, color(39, 54, 82));
        }
    }

    drawButton(15, 275, 74, 20,
               enrollmentNumericKeyboard ? "ABC" : "123", color(52, 73, 109));
    drawButton(96, 275, 68, 20, "a/A", color(52, 73, 109));
    drawButton(171, 275, 54, 20, enrollmentAccentCharacter(), color(52, 73, 109));
    drawButton(232, 275, 102, 20,
               text("Spacja", "Space", "Espacio", "Leer"), color(52, 73, 109));
    drawButton(341, 275, 124, 20,
               text("Usuń znak", "Backspace", "Borrar", "Löschen"),
               color(103, 47, 58));
    drawButton(65, 298, 150, 20,
               text("Zapisz", "Save", "Guardar", "Speichern"), color(25, 139, 103));
    drawButton(265, 298, 150, 20,
               text("Anuluj", "Cancel", "Cancelar", "Abbruch"), color(103, 47, 58));
}

void refreshHomeClock() {
    if (!rtcAvailable) return;
    const DateTime now = rtc.now();
    char clockText[10];
    snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());
    setUiTextSize(7.5f);
    display.setTextColor(color(248, 250, 255), color(22, 32, 54));
    display.drawCenterString(clockText, 240, 112);

    char dateText[20];
    snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d",
             now.year(), now.month(), now.day());
    if (lastHomeDate != dateText) {
        lastHomeDate = dateText;
        setUiTextSize(3.5f);
        display.setTextColor(color(174, 190, 215), color(22, 32, 54));
        display.drawCenterString(dateText, 240, 182);
    }
    if (lastHomeQuoteDate != dateText) drawHomeQuoteArea();
}

void drawActionChoice() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(selectedEmployee.firstName + " " + selectedEmployee.lastName,
                             240, 34);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1);
    display.drawCenterString(text("Wybierz czynność", "Choose an action",
                                  "Elija una acción", "Aktion wählen"),
                             240, 72);
    drawButton(30, 108, 200, 78,
               text("Wejście", "Clock in", "Entrada", "Kommen"),
               color(25, 139, 103));
    drawButton(250, 108, 200, 78,
               text("Wyjście", "Clock out", "Salida", "Gehen"),
               color(45, 101, 191));
    drawButton(30, 204, 420, 74,
               text("Odblokuj drzwi", "Unlock door", "Abrir puerta", "Tür öffnen"),
               color(164, 94, 34));
}

void drawPinEntry() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    const char* title = employeeEnrollmentRequested
                            ? text("PIN dodawania pracownika",
                                   "Employee registration PIN",
                                   "PIN de registro", "PIN zur Mitarbeiteraufnahme")
                            : screen == Screen::PinConfirm
                            ? text("Potwierdź nowy PIN", "Confirm new PIN",
                                   "Confirme el nuevo PIN", "Neue PIN bestätigen")
                            : screen == Screen::PinChange
                                  ? text("Wprowadź nowy PIN", "Enter new PIN",
                                         "Introduzca un nuevo PIN", "Neue PIN eingeben")
                                  : text("PIN ustawień", "Settings PIN", "PIN de ajustes",
                                         "Einstellungs-PIN");
    display.drawCenterString(title, 240, 18);
    setUiTextSize(2);
    String masked;
    for (size_t i = 0; i < pinInput.length(); ++i) masked += "* ";
    display.drawCenterString(masked, 240, 62);
    const char* keys[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "C", "0", "OK"};
    for (int index = 0; index < 12; ++index) {
        const int row = index / 3;
        const int column = index % 3;
        drawButton(84 + column * 106, 84 + row * 45, 92, 39, keys[index],
                   index == 11 ? color(25, 139, 103) : color(39, 54, 82));
    }
    drawButton(188, 270, 104, 38,
               text("Anuluj", "Cancel", "Cancelar", "Abbruch"),
               color(103, 47, 58));
}

void drawSettings() {
    static constexpr const char* settingLabels[][4] = {
        {"Język", "Language", "Idioma", "Sprache"},
        {"Tryb pracy", "Operating mode", "Modo de operación", "Betriebsmodus"},
        {"Zdjęcia", "Photos", "Fotos", "Fotos"},
        {"Czcionka", "Font", "Fuente", "Schriftart"},
        {"Pracownicy", "Employees", "Empleados", "Mitarbeiter"},
        {"Data i godzina", "Date and time", "Fecha y hora", "Datum und Uhrzeit"},
        {"Zmień PIN", "Change PIN", "Cambiar PIN", "PIN ändern"},
        {"Kalibruj dotyk", "Calibrate touch", "Calibrar pantalla",
         "Touchscreen kalibrieren"}
    };
    constexpr size_t kVisibleSettings = 5;
    constexpr int kSettingsRowY = 49;
    constexpr int kSettingsRowHeight = 43;
    constexpr size_t kTotalSettings = 8;
    const size_t maxScroll = kTotalSettings - kVisibleSettings;
    if (settingsScroll > maxScroll) settingsScroll = maxScroll;

    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(1.8f);
    display.drawCenterString(text("Ustawienia", "Settings", "Ajustes", "Einstellungen"),
                             240, 12);
    for (size_t row = 0; row < kVisibleSettings; ++row) {
        const size_t index = settingsScroll + row;
        const int y = kSettingsRowY + static_cast<int>(row) * kSettingsRowHeight;
        String label = settingLabels[index][static_cast<uint8_t>(config.language)];
        if (index == 0) {
            label += ": ";
            label += languageLabel();
        } else if (index == 1) {
            label += ": ";
            label += modeLabel();
        } else if (index == 2) {
            label += ": ";
            label += config.cameraEnabled
                         ? text("WŁ", "ON", "SÍ", "EIN")
                         : text("WYŁ", "OFF", "NO", "AUS");
            if (config.cameraEnabled) {
                label += cameraAvailable ? "  OK" : "  !";
            }
        } else if (index == 3) {
            label += ": ";
            label += uiFontLabel();
        } else if (index == 4) {
            label += " (" + String(activeEmployeeCount()) + ")";
        }
        drawButton(20, y, 420, 39, label,
                   index == 2 && config.cameraEnabled && !cameraAvailable
                       ? color(103, 67, 36)
                       : color(39, 54, 82));
    }
    drawVerticalScrollbar(462, kSettingsRowY, 5 * kSettingsRowHeight - 4,
                          kTotalSettings, kVisibleSettings, settingsScroll);
    drawButton(170, 274, 140, 38,
               text("Powrót", "Back", "Volver", "Zurück"),
               color(45, 101, 191));
    if (!lastMessage.isEmpty()) {
        setUiTextSize(0.6f);
        display.setTextColor(color(255, 191, 87));
        display.drawCenterString(lastMessage, 240, 262);
    }
}

size_t activeEmployeeCount() {
    size_t count = 0;
    for (const auto& employee : employees) {
        if (employee.active) ++count;
    }
    return count;
}

int activeEmployeeIndex(size_t visibleIndex) {
    size_t current = 0;
    for (size_t index = 0; index < employees.size(); ++index) {
        if (!employees[index].active) continue;
        if (current == visibleIndex) return static_cast<int>(index);
        ++current;
    }
    return -1;
}

void drawEmployeeList() {
    constexpr size_t kRowsPerPage = 4;
    const size_t count = activeEmployeeCount();
    const size_t pageCount =
        (count + kRowsPerPage - 1) / kRowsPerPage > 0
            ? (count + kRowsPerPage - 1) / kRowsPerPage
            : 1;
    if (employeeListPage >= pageCount) employeeListPage = pageCount - 1;

    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(1.8f);
    display.drawCenterString(text("Pracownicy", "Employees",
                                  "Empleados", "Mitarbeiter"),
                             240, 5);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1.1f);
    display.drawCenterString(String(employeeListPage + 1) + "/" +
                                 String(pageCount) + "  (" + String(count) + ")",
                             240, 29);

    if (count == 0) {
        display.setTextColor(color(174, 190, 215));
        setUiTextSize(1.6f);
        display.drawCenterString(text("Brak pracowników", "No employees",
                                      "No hay empleados", "Keine Mitarbeiter"),
                                 240, 128);
    }

    for (size_t row = 0; row < kRowsPerPage; ++row) {
        const int employeeIndex = activeEmployeeIndex(employeeListPage * kRowsPerPage + row);
        if (employeeIndex < 0) break;
        const Employee& employee = employees[employeeIndex];
        const int y = 47 + static_cast<int>(row) * 50;
        const uint16_t panelColor = color(22, 32, 54);
        display.fillRoundRect(16, y, 420, 46, 8, panelColor);
        setUiTextSize(1.4f);
        display.setTextColor(color(246, 249, 255));
        display.drawString(employee.firstName + " " + employee.lastName, 25, y + 5);
        display.setTextColor(color(174, 190, 215));
        setUiTextSize(1.2f);
        display.drawString("ID: " + employee.id, 25, y + 28);
        drawButton(254, y + 5, 116, 36,
                   text("Przekaźnik", "Relay", "Relé", "Relais"),
                   employee.relayAccess ? color(25, 139, 103) : color(150, 48, 58));
        drawButton(376, y + 5, 62, 36,
                   text("Usuń", "Remove", "Borrar", "Löschen"),
                   color(103, 47, 58));
    }
    drawVerticalScrollbar(462, 48, 196, count, kRowsPerPage,
                          employeeListPage * kRowsPerPage);
    if (!lastMessage.isEmpty()) {
        setUiTextSize(0.8f);
        display.setTextColor(color(255, 191, 87));
        display.drawCenterString(lastMessage, 240, 252);
    }

    drawButton(16, 267, 214, 43,
               text("Dodaj nowego", "Add employee",
                    "Añadir empleado", "Mitarbeiter hinzufügen"),
               color(25, 116, 88));
    drawButton(240, 267, 214, 43,
               text("-> Ustawienia", "-> Settings",
                    "-> Ajustes", "-> Einstellungen"),
               color(45, 101, 191));
}

void drawAddEmployeeCard() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(text("Dodawanie pracownika",
                                  "Add employee", "Añadir empleado",
                                  "Mitarbeiter hinzufügen"),
                             240, 72);
    display.setTextColor(color(174, 190, 215));
    setUiTextSize(1.4f);
    display.drawCenterString(text("Przyłóż nową kartę",
                                  "Scan the new card",
                                  "Acerque la nueva tarjeta",
                                  "Neue Karte vorhalten"),
                             240, 145);
    drawButton(170, 250, 140, 48,
               text("Anuluj", "Cancel", "Cancelar", "Abbrechen"),
               color(103, 47, 58));
}

bool solveTouchLeastSquares(const float rawX[4], const float rawY[4],
                            const int targetX[4], const int targetY[4],
                            TouchCalibration& result) {
    double normal[3][3] = {};
    double targetSums[2][3] = {};
    for (int i = 0; i < 4; ++i) {
        const double input[] = {rawX[i], rawY[i], 1.0};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                normal[row][column] += input[row] * input[column];
            }
            targetSums[0][row] += input[row] * targetX[i];
            targetSums[1][row] += input[row] * targetY[i];
        }
    }

    for (int axis = 0; axis < 2; ++axis) {
        double matrix[3][4] = {};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                matrix[row][column] = normal[row][column];
            }
            matrix[row][3] = targetSums[axis][row];
        }

        for (int column = 0; column < 3; ++column) {
            int pivot = column;
            for (int row = column + 1; row < 3; ++row) {
                if (fabs(matrix[row][column]) > fabs(matrix[pivot][column])) {
                    pivot = row;
                }
            }
            if (fabs(matrix[pivot][column]) < 1e-6) return false;
            if (pivot != column) {
                for (int j = column; j < 4; ++j) {
                    const double value = matrix[column][j];
                    matrix[column][j] = matrix[pivot][j];
                    matrix[pivot][j] = value;
                }
            }
            const double divisor = matrix[column][column];
            for (int j = column; j < 4; ++j) matrix[column][j] /= divisor;
            for (int row = 0; row < 3; ++row) {
                if (row == column) continue;
                const double factor = matrix[row][column];
                for (int j = column; j < 4; ++j) {
                    matrix[row][j] -= factor * matrix[column][j];
                }
            }
        }

        float* coefficients = axis == 0 ? result.x : result.y;
        for (int i = 0; i < 3; ++i) {
            coefficients[i] = static_cast<float>(matrix[i][3]);
            if (!isfinite(coefficients[i])) return false;
        }
    }

    if (fabs(result.x[0]) > 2.0f || fabs(result.x[1]) > 2.0f ||
        fabs(result.y[0]) > 2.0f || fabs(result.y[1]) > 2.0f ||
        fabs(result.x[2]) > 8000.0f || fabs(result.y[2]) > 8000.0f) {
        return false;
    }
    result.magic = kTouchCalibrationMagic;
    return true;
}

bool loadTouchCalibration() {
    TouchCalibration candidate;
    if (!preferences.isKey("touch_v1") ||
        preferences.getBytes("touch_v1", &candidate,
                             sizeof(candidate)) != sizeof(candidate) ||
        candidate.magic != kTouchCalibrationMagic) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(candidate.x[i]) || !isfinite(candidate.y[i])) {
            return false;
        }
    }
    touchCalibration = candidate;
    return true;
}

bool captureCalibrationPoint(float& rawX, float& rawY) {
    constexpr uint32_t kPointTimeoutMs = 30000;
    const uint32_t started = millis();
    lgfx::touch_point_t raw;
    while (millis() - started < kPointTimeoutMs) {
        if (display.getTouchRaw(&raw)) break;
        delay(20);
    }
    if (millis() - started >= kPointTimeoutMs) return false;

    uint32_t sumX = 0;
    uint32_t sumY = 0;
    uint8_t samples = 0;
    while (samples < 12) {
        if (display.getTouchRaw(&raw)) {
            sumX += static_cast<uint16_t>(raw.x);
            sumY += static_cast<uint16_t>(raw.y);
            ++samples;
        } else if (samples > 0) {
            break;
        }
        delay(12);
    }
    while (display.getTouchRaw(&raw)) delay(20);
    if (samples < 4) return false;
    rawX = static_cast<float>(sumX) / samples;
    rawY = static_cast<float>(sumY) / samples;
    return true;
}

bool runTouchCalibration() {
    static constexpr int targetX[] = {
        kTouchCalibrationMargin, kScreenWidth - kTouchCalibrationMargin - 1,
        kScreenWidth - kTouchCalibrationMargin - 1, kTouchCalibrationMargin
    };
    static constexpr int targetY[] = {
        kTouchCalibrationMargin, kTouchCalibrationMargin,
        kScreenHeight - kTouchCalibrationMargin - 1,
        kScreenHeight - kTouchCalibrationMargin - 1
    };
    float rawX[4] = {};
    float rawY[4] = {};

    for (int i = 0; i < 4; ++i) {
        display.fillScreen(color(10, 17, 33));
        display.setTextColor(color(246, 249, 255));
        setUiTextSize(2);
        display.drawCenterString(
            text("Kalibracja dotyku", "Touch calibration",
                 "Calibración táctil", "Touch-Kalibrierung"),
            240, 62);
        setUiTextSize(1);
        display.setTextColor(color(174, 190, 215));
        display.drawCenterString(
            text("Dotknij i przytrzymaj każdy punkt",
                 "Touch and hold each target",
                 "Toque y mantenga cada punto",
                 "Jeden Punkt berühren und halten"),
            240, 104);
        display.drawCenterString(
            String(i + 1) + "/4",
            240, 126);
        display.drawCircle(targetX[i], targetY[i], 12, color(79, 146, 255));
        display.fillCircle(targetX[i], targetY[i], 3, color(255, 191, 87));

        if (!captureCalibrationPoint(rawX[i], rawY[i])) {
            Serial.println("Touch calibration timed out");
            return false;
        }
    }

    TouchCalibration candidate;
    if (!solveTouchLeastSquares(rawX, rawY, targetX, targetY, candidate)) {
        display.fillScreen(color(10, 17, 33));
        display.setTextColor(color(255, 150, 150));
        setUiTextSize(2);
        display.drawCenterString(
            text("Kalibracja nieudana", "Calibration failed",
                 "Calibración fallida", "Kalibrierung fehlgeschlagen"),
            240, 130);
        delay(1200);
        return false;
    }

    touchCalibration = candidate;
    if (preferences.putBytes("touch_v1", &touchCalibration,
                            sizeof(touchCalibration)) != sizeof(touchCalibration)) {
        Serial.println("Could not save touch calibration");
    }
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(103, 220, 163));
    setUiTextSize(2);
    display.drawCenterString(
        text("Kalibracja zapisana", "Calibration saved",
             "Calibración guardada", "Kalibrierung gespeichert"),
        240, 145);
    delay(700);
    return true;
}

bool getCalibratedTouch(int32_t& x, int32_t& y) {
    if (touchCalibration.magic != kTouchCalibrationMagic) {
        return display.getTouch(&x, &y);
    }
    lgfx::touch_point_t raw;
    if (!display.getTouchRaw(&raw)) return false;
    x = lroundf(touchCalibration.x[0] * raw.x +
                touchCalibration.x[1] * raw.y + touchCalibration.x[2]);
    y = lroundf(touchCalibration.y[0] * raw.x +
                touchCalibration.y[1] * raw.y + touchCalibration.y[2]);
    x = constrain(x, 0, static_cast<int32_t>(kScreenWidth - 1));
    y = constrain(y, 0, static_cast<int32_t>(kScreenHeight - 1));
    return true;
}

int daysInMonth(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        return 29;
    }
    return days[month - 1];
}

void drawClockSettings() {
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(246, 249, 255));
    setUiTextSize(2);
    display.drawCenterString(text("Ustaw datę i godzinę", "Set date and time",
                                  "Ajustar fecha y hora", "Datum und Uhrzeit einstellen"),
                             240, 20);
    const DateTime now = rtc.now();
    const String values[] = {
        String(text("Godzina: ", "Hour: ", "Hora: ", "Stunde: ")) + String(now.hour()),
        String(text("Minuta: ", "Minute: ", "Minuto: ", "Minute: ")) + String(now.minute()),
        String(text("Dzień: ", "Day: ", "Día: ", "Tag: ")) + String(now.day()),
        String(text("Miesiąc: ", "Month: ", "Mes: ", "Monat: ")) + String(now.month()),
        String(text("Rok: ", "Year: ", "Año: ", "Jahr: ")) + String(now.year()),
    };
    for (int row = 0; row < 5; ++row) {
        const int y = 58 + row * 42;
        setUiTextSize(2);
        display.setTextColor(color(221, 230, 246));
        display.drawString(values[row], 64, y + 8);
        drawButton(300, y, 60, 38, "-", color(39, 54, 82));
        drawButton(372, y, 60, 38, "+", color(39, 54, 82));
    }
    drawButton(188, 274, 104, 38,
               text("Gotowe", "Done", "Listo", "Fertig"), color(45, 101, 191));
}

Employee* findEmployee(const String& uid) {
    for (auto& employee : employees) {
        if (employee.active && employee.uid == uid) return &employee;
    }
    return nullptr;
}

bool saveNewEmployee() {
    newEmployeeId.trim();
    newEmployeeFirstName.trim();
    newEmployeeLastName.trim();
    if (newEmployeeId.isEmpty() || newEmployeeFirstName.isEmpty() ||
        newEmployeeLastName.isEmpty() || pendingEmployeeUid.isEmpty()) {
        lastMessage = text("Uzupełnij ID, imię i nazwisko",
                           "Enter ID, first and last name",
                           "Introduzca ID, nombre y apellido",
                           "ID, Vor- und Nachname eingeben");
        drawEmployeeEnrollment();
        return false;
    }
    if (newEmployeeId.length() > 32 || newEmployeeFirstName.length() > 32 ||
        newEmployeeLastName.length() > 32) {
        lastMessage = text("Każde pole może mieć maks. 32 znaki",
                           "Each field is limited to 32 characters",
                           "Cada campo admite 32 caracteres",
                           "Jedes Feld darf höchstens 32 Zeichen haben");
        drawEmployeeEnrollment();
        return false;
    }
    for (const auto& existing : employees) {
        if (existing.id.equalsIgnoreCase(newEmployeeId) ||
            existing.uid.equalsIgnoreCase(pendingEmployeeUid)) {
            lastMessage = text("ID lub karta już istnieje",
                               "ID or card already exists",
                               "El ID o la tarjeta ya existe",
                               "ID oder Karte bereits vorhanden");
            drawEmployeeEnrollment();
            return false;
        }
    }
    if (employees.size() >= kMaximumEmployees) {
        lastMessage = text("Osiągnięto limit pracowników",
                           "Employee limit reached",
                           "Límite de empleados alcanzado",
                           "Mitarbeiterlimit erreicht");
        drawEmployeeEnrollment();
        return false;
    }

    Employee employee;
    employee.id = newEmployeeId;
    employee.uid = pendingEmployeeUid;
    employee.firstName = newEmployeeFirstName;
    employee.lastName = newEmployeeLastName;
    employee.active = true;
    employee.relayAccess = newEmployeeRelayAccess;

    if (config.mode == Mode::Online) {
        JsonDocument request;
        request["action"] = "registerEmployee";
        JsonObject row = request["row"].to<JsonObject>();
        row["employee_id"] = employee.id;
        row["card_uid"] = employee.uid;
        row["first_name"] = employee.firstName;
        row["last_name"] = employee.lastName;
        row["active"] = true;
        row["relay_access"] = employee.relayAccess;
        if (!postToAppsScript(request)) {
            lastMessage = text("Błąd zapisu w arkuszu Google",
                               "Could not save to Google Sheets",
                               "No se pudo guardar en Google Sheets",
                               "Speichern in Google Sheets fehlgeschlagen");
            drawEmployeeEnrollment();
            return false;
        }
    } else {
        if (!sdAvailable || !offlineEmployeesLoaded) {
            lastMessage = text("Brak gotowego pliku employees.csv",
                               "employees.csv is unavailable",
                               "employees.csv no está disponible",
                               "employees.csv ist nicht verfügbar");
            drawEmployeeEnrollment();
            return false;
        }
        constexpr char temporaryPath[] = "/employees.csv.tmp";
        constexpr char backupPath[] = "/employees.csv.bak";
        SD.remove(temporaryPath);
        SD.remove(backupPath);
        File file = SD.open(temporaryPath, FILE_WRITE);
        if (!file) {
            lastMessage = text("Nie można zapisać employees.csv",
                               "Could not write employees.csv",
                               "No se pudo escribir employees.csv",
                               "employees.csv konnte nicht geschrieben werden");
            drawEmployeeEnrollment();
            return false;
        }
        bool writeOk =
            file.println("employee_id,card_uid,first_name,last_name,active,relay_access") > 0;
        auto writeEmployee = [&file, &writeOk](const Employee& row) {
            if (!writeOk) return;
            String csvRow = csvEscape(row.id) + "," + csvEscape(row.uid) + "," +
                            csvEscape(row.firstName) + "," + csvEscape(row.lastName) +
                            "," + (row.active ? "true" : "false") + "," +
                            (row.relayAccess ? "true" : "false") + "\n";
            writeOk = file.print(csvRow) == csvRow.length();
        };
        for (const auto& existing : employees) {
            writeEmployee(existing);
        }
        writeEmployee(employee);
        file.flush();
        file.close();
        if (!writeOk) {
            SD.remove(temporaryPath);
            lastMessage = text("Błąd zapisu employees.csv",
                               "Failed to write employees.csv",
                               "Error al escribir employees.csv",
                               "Fehler beim Schreiben von employees.csv");
            drawEmployeeEnrollment();
            return false;
        }
        const bool hadOriginal = SD.exists(kEmployeesPath);
        if (hadOriginal && !SD.rename(kEmployeesPath, backupPath)) {
            SD.remove(temporaryPath);
            lastMessage = text("Nie można zabezpieczyć employees.csv",
                               "Could not back up employees.csv",
                               "No se pudo respaldar employees.csv",
                               "employees.csv konnte nicht gesichert werden");
            drawEmployeeEnrollment();
            return false;
        }
        if (!SD.rename(temporaryPath, kEmployeesPath)) {
            if (hadOriginal) SD.rename(backupPath, kEmployeesPath);
            SD.remove(temporaryPath);
            lastMessage = text("Nie można zatwierdzić employees.csv",
                               "Could not replace employees.csv",
                               "No se pudo reemplazar employees.csv",
                               "employees.csv konnte nicht ersetzt werden");
            drawEmployeeEnrollment();
            return false;
        }
        SD.remove(backupPath);
    }

    employees.push_back(employee);
    if (config.mode == Mode::Online) onlineEmployeesLoaded = true;
    addLog("INFO", "EMPLOYEE_REGISTERED", employee.id + ":" + employee.uid);
    lastMessage = text("Dodano: ", "Added: ", "Añadido: ", "Hinzugefügt: ") +
                  employee.firstName + " " + employee.lastName;
    const bool returnToList = employeeManagementReturnToList;
    employeeManagementReturnToList = false;
    pendingEmployeeUid = "";
    screen = returnToList ? Screen::EmployeeList : Screen::Home;
    if (returnToList) drawEmployeeList();
    else drawHome();
    return true;
}

void handleCard() {
    if (!rfidAvailable || !rfid.PICC_IsNewCardPresent() ||
        !rfid.PICC_ReadCardSerial()) return;
    String uid;
    for (byte index = 0; index < rfid.uid.size; ++index) {
        if (rfid.uid.uidByte[index] < 0x10) uid += '0';
        uid += String(rfid.uid.uidByte[index], HEX);
    }
    uid.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    const uint32_t now = millis();
    if (uid == lastCardUid && now - lastCardAt < kRfidDebounceMs) return;
    lastCardUid = uid;
    lastCardAt = now;

    if (!configurationValid ||
        (config.mode == Mode::Offline && !offlineEmployeesLoaded) ||
        (config.mode == Mode::Online && !onlineEmployeesLoaded)) {
        lastMessage = text("Brak konfiguracji lub listy pracowników",
                           "Configuration or employee list unavailable",
                           "Configuración o empleados no disponibles",
                           "Konfiguration oder Mitarbeiterliste fehlt");
        drawHome();
        return;
    }

    Employee* employee = findEmployee(uid);
    if (screen == Screen::AddEmployeeCard) {
        if (employee != nullptr) {
            lastMessage = text("Ta karta jest już przypisana",
                               "This card is already assigned",
                               "Esta tarjeta ya está asignada",
                               "Diese Karte ist bereits zugeordnet");
            drawAddEmployeeCard();
            display.setTextColor(color(255, 191, 87));
            setUiTextSize(0.9f);
            display.drawCenterString(lastMessage, 240, 205);
            return;
        }
        pendingEmployeeUid = uid;
        employeeEnrollmentRequested = true;
        pinInput = "";
        screenSince = millis();
        screen = Screen::PinEntry;
        drawPinEntry();
        return;
    }
    if (employee == nullptr) {
        pendingEmployeeUid = uid;
        employeeEnrollmentRequested = false;
        screenSince = millis();
        screen = Screen::UnknownCard;
        drawUnknownCard();
        Serial.printf("Unknown RFID card: %s\n", uid.c_str());
        return;
    }
    if (!rtcAvailable) {
        lastMessage = text("Brak zegara RTC — odbicie anulowane",
                           "RTC unavailable — punch rejected",
                           "RTC no disponible — registro rechazado",
                           "RTC nicht verfügbar — Buchung abgelehnt");
        addLog("ERROR", "RTC_UNAVAILABLE", uid);
        drawHome();
        return;
    }
    selectedEmployee = *employee;
    if (employee->relayAccess) {
        screen = Screen::ActionChoice;
        drawActionChoice();
        return;
    }

    if (config.mode == Mode::Online) {
        processAttendance(AttendanceAction::Entry, true);
    } else {
        String previous;
        if (!previousAttendance(employee->id, previous)) {
            lastMessage = text("Brak karty SD — odbicie anulowane",
                               "SD unavailable — punch rejected",
                               "SD no disponible — registro rechazado",
                               "SD-Karte fehlt — Buchung abgelehnt");
            drawHome();
            return;
        }
        processAttendance(previous == "ENTRY" ? AttendanceAction::Exit
                                              : AttendanceAction::Entry);
    }
}

void handlePinTouch(int32_t x, int32_t y) {
    if (y >= 270 && x >= 188 && x <= 292) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pinInput = "";
        employeeEnrollmentRequested = false;
        pendingEmployeeUid = "";
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
        return;
    }
    if (y >= 84 && y < 264) {
        const int column = (x - 84) / 106;
        const int row = (y - 84) / 45;
        if (column < 0 || column > 2 || row < 0 || row > 3) return;
        const int key = row * 3 + column;
        if (key == 9) {
            pinInput = "";
        } else if (key == 11) {
            if (pinInput.length() != 4) return;
            if (screen == Screen::PinChange) {
                pendingPin = pinInput;
                pinInput = "";
                screen = Screen::PinConfirm;
            } else if (screen == Screen::PinConfirm) {
                if (pinInput == pendingPin) {
                    storedPin = pendingPin;
                    preferences.putString("pin", storedPin);
                    addLog("INFO", "PIN_CHANGED", "");
                    screen = Screen::Settings;
                } else {
                    pinInput = "";
                    pendingPin = "";
                    screen = Screen::PinChange;
                    lastMessage = text("PIN nie pasuje — spróbuj ponownie",
                                       "PINs did not match — retry",
                                       "Los PIN no coinciden — inténtelo de nuevo",
                                       "PINs stimmen nicht überein");
                }
            } else if (pinInput == storedPin) {
                if (employeeEnrollmentRequested) {
                    employeeEnrollmentRequested = false;
                    newEmployeeId = "";
                    newEmployeeFirstName = "";
                    newEmployeeLastName = "";
                    newEmployeeRelayAccess = false;
                    enrollmentNumericKeyboard = false;
                    enrollmentField = 0;
                    enrollmentAccentIndex = 0;
                    enrollmentLowercase = false;
                    lastMessage = "";
                    screen = Screen::EmployeeEnroll;
                    screenSince = millis();
                    drawEmployeeEnrollment();
                } else {
                    screen = Screen::Settings;
                    drawSettings();
                }
                pinInput = "";
            } else {
                pinInput = "";
                lastMessage = text("Nieprawidłowy PIN", "Incorrect PIN",
                                   "PIN incorrecto", "Falsche PIN");
                addLog("WARN", "PIN_REJECTED", "");
            }
        } else if (pinInput.length() < 4) {
            const char* digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9",
                                    "", "0", ""};
            pinInput += digits[key];
        }
        if (screen == Screen::PinEntry || screen == Screen::PinChange ||
            screen == Screen::PinConfirm) {
            drawPinEntry();
        }
    }
}

void handleEmployeeEnrollmentTouch(int32_t x, int32_t y) {
    if (y >= 298) {
        if (x >= 65 && x < 215) {
            saveNewEmployee();
        } else if (x >= 265 && x < 415) {
            pendingEmployeeUid = "";
            employeeEnrollmentRequested = false;
            screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
            lastMessage = "";
            if (employeeManagementReturnToList) drawEmployeeList();
            else drawHome();
        }
        return;
    }
    if (y >= 48 && y < 144) {
        enrollmentField = static_cast<uint8_t>((y - 48) / 32);
        enrollmentLowercase = enrollmentField != 0;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 150 && y < 175) {
        newEmployeeRelayAccess = !newEmployeeRelayAccess;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 179 && y < 272) {
        const int row = (y - 179) / 33;
        if (row < 0 || row > 2) return;
        const int rowStarts[] = {20, 42, 86};
        const int rowLengths[] = {10, 9, 7};
        const char* alphaRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
        const char* numberRows[] = {"1234567890", "0987654321", ".,-_@"};
        const int numberLengths[] = {10, 10, 5};
        const int start = enrollmentNumericKeyboard ? 20 : rowStarts[row];
        const int keyIndex = (x - start) / 44;
        const int keyCount = enrollmentNumericKeyboard ? numberLengths[row]
                                                       : rowLengths[row];
        if (x < start || keyIndex < 0 || keyIndex >= keyCount) return;
        String key = enrollmentNumericKeyboard
                         ? String(numberRows[row][keyIndex])
                         : String(alphaRows[row][keyIndex]);
        if (!enrollmentNumericKeyboard && enrollmentLowercase) key.toLowerCase();
        String* value = enrollmentFieldValue(enrollmentField);
        if (value->length() < 32) *value += key;
        screenSince = millis();
        drawEmployeeEnrollment();
        return;
    }
    if (y >= 275 && y < 296) {
        String* value = enrollmentFieldValue(enrollmentField);
        if (x < 90) {
            enrollmentNumericKeyboard = !enrollmentNumericKeyboard;
        } else if (x < 165) {
            enrollmentLowercase = !enrollmentLowercase;
        } else if (x < 230) {
            const char* accent = enrollmentAccentCharacter();
            if (value->length() + strlen(accent) <= 32) *value += accent;
            enrollmentAccentIndex++;
        } else if (x < 340) {
            if (value->length() < 32) *value += ' ';
        } else {
            removeLastInputCharacter(*value);
        }
        screenSince = millis();
        drawEmployeeEnrollment();
    }
}

void switchMode() {
    const Mode previous = config.mode;
    config.mode = config.mode == Mode::Offline ? Mode::Online : Mode::Offline;
    if (!saveConfig()) {
        config.mode = previous;
        return;
    }
    employees.clear();
    onlineEmployeesLoaded = false;
    offlineEmployeesLoaded = false;
    onlineEmployeesLoaded = false;
    if (config.mode == Mode::Offline) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        loadEmployeesFromCsv();
        lastMessage = text("Tryb offline", "Offline mode", "Modo sin conexión",
                           "Offline-Modus");
    } else if (config.ssid.isEmpty()) {
        lastMessage = text("Brak konfiguracji Wi-Fi", "Wi-Fi is not configured",
                           "Wi-Fi no está configurado", "WLAN nicht konfiguriert");
    } else {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        const uint32_t startedAt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 10000) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED && loadEmployeesFromOnlineSheet()) {
            nextEmployeeRefreshAt = millis() + kEmployeeRefreshMs;
            lastMessage = text("Tryb online", "Online mode", "Modo en línea",
                               "Online-Modus");
        } else {
            lastMessage = text("Brak API — tryb online nieaktywny",
                               "API unavailable — online mode inactive",
                               "API no disponible — modo inactivo",
                               "API nicht erreichbar — Online-Modus inaktiv");
            addLog("ERROR", "ONLINE_MODE_UNAVAILABLE", "");
        }
    }
}

void setCameraEnabled(bool enabled) {
    if (enabled && !cameraAvailable) cameraAvailable = initCamera();
    if (!enabled && cameraAvailable) {
        const esp_err_t result = esp_camera_deinit();
        if (result != ESP_OK) {
            addLog("ERROR", "CAMERA_DEINIT_FAILED", String(result));
            lastMessage = text("Nie można wyłączyć aparatu",
                               "Could not stop camera",
                               "No se pudo detener la cámara",
                               "Kamera konnte nicht beendet werden");
            return;
        }
        cameraAvailable = false;
    }
    config.cameraEnabled = enabled;
    if (preferences.putBool("camera", enabled) == 0) {
        Serial.println("Could not persist camera setting in NVS");
        lastMessage = text("Nie zapisano ustawienia aparatu",
                           "Could not save camera setting",
                           "No se pudo guardar la cámara",
                           "Kameraeinstellung konnte nicht gespeichert werden");
        return;
    }
    if (sdAvailable && !saveConfig()) {
        Serial.println("Could not update camera_enabled in config.json");
        lastMessage = text("Nie zapisano config.json",
                           "Could not update config.json",
                           "No se pudo actualizar config.json",
                           "config.json konnte nicht aktualisiert werden");
    } else if (enabled && !cameraAvailable) {
        Serial.printf("Photo mode enabled, but camera init failed: 0x%x\n",
                      cameraInitError);
        addLog("ERROR", "CAMERA_INIT_FAILED",
               "esp_camera_init failed: 0x" +
                   String(static_cast<uint32_t>(cameraInitError), HEX));
        lastMessage = text("Tryb zdjęć ON, aparat niedostępny",
                           "Photos ON, camera unavailable",
                           "Fotos activadas, cámara no disponible",
                           "Fotos EIN, Kamera nicht verfügbar");
    } else {
        lastMessage = "";
    }
}

void adjustClock(int field, int delta) {
    if (!rtcAvailable) return;
    const DateTime current = rtc.now();
    int year = current.year();
    int month = current.month();
    int day = current.day();
    int hour = current.hour();
    int minute = current.minute();
    switch (field) {
        case 0:
            hour = (hour + delta + 24) % 24;
            break;
        case 1:
            minute = (minute + delta + 60) % 60;
            break;
        case 2:
            day = constrain(day + delta, 1, daysInMonth(year, month));
            break;
        case 3:
            month = constrain(month + delta, 1, 12);
            day = min(day, daysInMonth(year, month));
            break;
        case 4:
            year = constrain(year + delta, 2000, 2099);
            day = min(day, daysInMonth(year, month));
            break;
        default:
            return;
    }
    rtc.adjust(DateTime(year, month, day, hour, minute, current.second()));
}

void handleTouch() {
    int32_t x = 0;
    int32_t y = 0;
    static bool touchWasDown = false;
    if (!getCalibratedTouch(x, y)) {
        touchWasDown = false;
        return;
    }
    if (touchWasDown) return;
    touchWasDown = true;
    if (screen == Screen::Home) {
        if (x >= 350 && y >= 260) {
            screen = Screen::PinEntry;
            pinInput = "";
            screenSince = millis();
            drawPinEntry();
        }
    } else if (screen == Screen::UnknownCard) {
        if (y >= 215 && y < 277 && x < 240) {
            employeeEnrollmentRequested = true;
            pinInput = "";
            screenSince = millis();
            screen = Screen::PinEntry;
            drawPinEntry();
        } else if (y >= 215 && y < 277 && x >= 240) {
            pendingEmployeeUid = "";
            employeeEnrollmentRequested = false;
            lastMessage = "";
            screen = Screen::Home;
            drawHome();
        }
    } else if (screen == Screen::ActionChoice) {
        if (y >= 108 && y < 190 && x < 240) {
            processAttendance(AttendanceAction::Entry);
        } else if (y >= 108 && y < 190 && x >= 240) {
            processAttendance(AttendanceAction::Exit);
        } else if (y >= 204 && y < 282) {
            setRelay(true);
            lastMessage = text("Drzwi odblokowane", "Door unlocked",
                               "Puerta desbloqueada", "Tür entriegelt");
            screen = Screen::Home;
            drawHome();
        }
    } else if (screen == Screen::PinEntry || screen == Screen::PinChange ||
               screen == Screen::PinConfirm) {
        handlePinTouch(x, y);
    } else if (screen == Screen::EmployeeEnroll) {
        handleEmployeeEnrollmentTouch(x, y);
    } else if (screen == Screen::Settings) {
        if (y >= 274 && x >= 170 && x < 310) {
            employeeManagementReturnToList = false;
            screen = Screen::Home;
            drawHome();
            return;
        }
        if (x >= 440 && y >= 49 && y < 264) {
            settingsScroll = static_cast<uint8_t>(
                (static_cast<uint32_t>(y - 49) * 3) / 213);
            drawSettings();
            return;
        }
        if (y >= 49 && y < 264) {
            const size_t settingIndex =
                settingsScroll + static_cast<size_t>((y - 49) / 43);
            switch (settingIndex) {
                case 0:
                    config.language = static_cast<Language>(
                        (static_cast<uint8_t>(config.language) + 1) % 4);
                    preferences.putUChar("language",
                                         static_cast<uint8_t>(config.language));
                    break;
                case 1:
                    switchMode();
                    break;
                case 2:
                    setCameraEnabled(!config.cameraEnabled);
                    break;
                case 3:
                    uiFont = static_cast<UiFont>(
                        (static_cast<uint8_t>(uiFont) + 1) % 4);
                    applyUiFont();
                    if (preferences.putUChar("font",
                                             static_cast<uint8_t>(uiFont)) == 0) {
                        lastMessage = text("Nie zapisano czcionki",
                                           "Could not save font",
                                           "No se pudo guardar la fuente",
                                           "Schriftart konnte nicht gespeichert werden");
                    } else {
                        lastMessage =
                            uiFont == UiFont::Unicode
                                ? ""
                                : text("Ta czcionka może nie wyświetlać polskich znaków",
                                       "This font may not display accented characters",
                                       "Esta fuente puede omitir caracteres acentuados",
                                       "Diese Schrift zeigt möglicherweise keine Umlaute");
                    }
                    break;
                case 4:
                    employeeListPage = 0;
                    lastMessage = "";
                    screen = Screen::EmployeeList;
                    drawEmployeeList();
                    return;
                case 5:
                    screen = Screen::ClockSet;
                    drawClockSettings();
                    return;
                case 6:
                    screen = Screen::PinChange;
                    pinInput = "";
                    screenSince = millis();
                    drawPinEntry();
                    return;
                case 7:
                    runTouchCalibration();
                    screen = Screen::Settings;
                    drawSettings();
                    return;
                default:
                    return;
            }
            drawSettings();
        }
    } else if (screen == Screen::EmployeeList) {
        constexpr size_t kRowsPerPage = 4;
        const size_t count = activeEmployeeCount();
        const size_t pageCount =
            (count + kRowsPerPage - 1) / kRowsPerPage > 0
                ? (count + kRowsPerPage - 1) / kRowsPerPage
                : 1;
        if (x >= 438 && y >= 48 && y < 244) {
            employeeListPage = static_cast<uint16_t>(
                (static_cast<uint32_t>(y - 48) * (pageCount - 1)) / 195);
        } else if (y >= 47 && y < 247) {
            const size_t row = static_cast<size_t>((y - 47) / 50);
            const int employeeIndex =
                activeEmployeeIndex(employeeListPage * kRowsPerPage + row);
            if (employeeIndex >= 0) {
                const Employee employee = employees[employeeIndex];
                if (x >= 254 && x < 374) {
                    updateEmployeeRecord(static_cast<size_t>(employeeIndex),
                                         employee.active, !employee.relayAccess);
                } else if (x >= 376 && x < 440) {
                    updateEmployeeRecord(static_cast<size_t>(employeeIndex),
                                         false, employee.relayAccess);
                }
            }
        } else if (y >= 267 && y < 312 && x >= 16 && x < 230) {
            employeeManagementReturnToList = true;
            employeeEnrollmentRequested = false;
            lastMessage = "";
            screenSince = millis();
            screen = Screen::AddEmployeeCard;
            drawAddEmployeeCard();
            return;
        } else if (y >= 267 && y < 312 && x >= 240 && x < 460) {
            employeeManagementReturnToList = false;
            lastMessage = "";
            screen = Screen::Settings;
            drawSettings();
            return;
        }
        drawEmployeeList();
    } else if (screen == Screen::AddEmployeeCard) {
        if (y >= 250 && y < 300 && x >= 170 && x < 310) {
            employeeManagementReturnToList = false;
            lastMessage = "";
            screen = Screen::EmployeeList;
            drawEmployeeList();
        }
    } else if (screen == Screen::ClockSet) {
        if (y >= 274) {
            screen = Screen::Settings;
            drawSettings();
        } else if (x >= 300 && x < 432 && y >= 58 && y < 268) {
            const int row = (y - 58) / 42;
            adjustClock(row, x >= 372 ? 1 : -1);
            drawClockSettings();
        }
    }
    delay(180);
}

bool initDisplay() {
    pinMode(pins::rfid_rst, OUTPUT);
    digitalWrite(pins::rfid_rst, LOW);
    delay(20);
    digitalWrite(pins::rfid_rst, HIGH);
    delay(20);
    if (!display.init()) return false;
    display.setColorDepth(16);
    applyUiFont();
    display.setRotation(1);
    display.fillScreen(color(10, 17, 33));
    display.setTextColor(color(245, 248, 255));
    setUiTextSize(3);
    display.drawCenterString("RCP2027", 240, 136);
    setUiTextSize(1);
    display.setTextColor(color(145, 160, 185));
    display.drawCenterString("Inicjalizacja terminala", 240, 180);
    return true;
}

void setupRcp() {
    Serial.begin(115200);
    const bool displayReady = initDisplay();
    if (!displayReady) {
        Serial.println("ILI9488 initialization failed");
    }

    preferences.begin("rcp", false);
    storedPin = preferences.getString("pin", "0000");
    const bool savedLanguage = preferences.isKey("language");
    const bool savedCamera = preferences.isKey("camera");
    const uint8_t savedFont = preferences.getUChar("font", 0);
    if (savedFont <= static_cast<uint8_t>(UiFont::FreeMono)) {
        uiFont = static_cast<UiFont>(savedFont);
    } else {
        preferences.remove("font");
    }
    applyUiFont();
    if (loadTouchCalibration()) {
        Serial.println("Loaded touchscreen calibration from NVS");
    }

    Wire.begin(pins::i2c_sda, pins::i2c_scl);
    rtcAvailable = rtc.begin(&Wire);
    if (rtcAvailable && rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("RTC lost power; set to firmware build time");
    }

    pinMode(pins::relay, OUTPUT_OPEN_DRAIN);
    digitalWrite(pins::relay, HIGH);
    esp_timer_create_args_t relayTimerConfig = {};
    relayTimerConfig.callback = relayTimerCallback;
    relayTimerConfig.name = "rcp-relay-close";
    if (esp_timer_create(&relayTimerConfig, &relayTimer) != ESP_OK) {
        relayTimer = nullptr;
        Serial.println("Relay safety timer creation failed; using loop timeout");
    }
    pinMode(pins::buzzer, OUTPUT);
    digitalWrite(pins::buzzer, LOW);

    sdBus.begin(pins::tft_sck, pins::tft_miso, pins::tft_mosi, pins::sd_cs);
    sdAvailable = SD.begin(pins::sd_cs, sdBus, 20000000);
    if (sdAvailable) {
        const bool employeesFileReady = ensureCsvFile(
            kEmployeesPath,
            "employee_id,card_uid,first_name,last_name,active,relay_access");
        const bool attendanceFileReady = ensureCsvFile(
            kAttendancePath,
            "event_id,timestamp,employee_id,card_uid,event_type,photo_file,source,notes");
        const bool logsFileReady = ensureCsvFile(
            kLogsPath, "event_id,timestamp,level,code,message");
        sdFilesInitialized = employeesFileReady && attendanceFileReady && logsFileReady;

        if (!SD.exists(kConfigPath)) {
            config.mode = Mode::Offline;
            config.language = Language::Polish;
            configurationValid = saveConfig();
            if (configurationValid) {
                Serial.println("Created default offline config.json");
            } else {
                Serial.println("Could not create default offline config.json");
            }
        } else if (!loadConfig()) {
            addLog("ERROR", "CONFIG_INVALID", "Invalid config.json; refusing defaults");
        }
    } else {
        Serial.println("SD initialization failed");
    }
    if (savedLanguage) {
        const uint8_t saved = preferences.getUChar("language", 0);
        if (saved <= static_cast<uint8_t>(Language::German)) {
            config.language = static_cast<Language>(saved);
        } else {
            preferences.remove("language");
        }
    }
    if (savedCamera) config.cameraEnabled = preferences.getBool("camera", false);
    if (displayReady && touchCalibration.magic != kTouchCalibrationMagic) {
        runTouchCalibration();
    }

    SPI.begin(pins::rfid_sck, pins::rfid_miso, pins::rfid_mosi, pins::rfid_cs);
    rfid.PCD_Init();
    const byte rfidVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    rfidAvailable = rfidVersion != 0x00 && rfidVersion != 0xFF;
    Serial.printf("MFRC522 VersionReg=0x%02X\n", rfidVersion);
    if (rfidAvailable) {
        rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    } else {
        Serial.println("MFRC522 did not respond over SPI; check power, reset and SPI pins");
    }

    if (config.cameraEnabled) {
        cameraAvailable = initCamera();
        if (!cameraAvailable) {
            addLog("ERROR", "CAMERA_INIT_FAILED",
                   "esp_camera_init failed: 0x" +
                       String(static_cast<uint32_t>(cameraInitError), HEX));
        }
    }
    if (configurationValid && config.mode == Mode::Online && !config.ssid.isEmpty()) {
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.begin(config.ssid.c_str(), config.wifiPassword.c_str());
        const uint32_t startedAt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 10000) {
            delay(100);
        }
        if (WiFi.status() != WL_CONNECTED) {
            addLog("ERROR", "WIFI_CONNECT_FAILED", WiFi.SSID());
        } else if (!loadEmployeesFromOnlineSheet()) {
            addLog("ERROR", "EMPLOYEE_SYNC_FAILED", "Could not load employees");
        } else {
            nextEmployeeRefreshAt = millis() + kEmployeeRefreshMs;
        }
    } else if (configurationValid && config.mode == Mode::Offline) {
        loadEmployeesFromCsv();
    }

    digitalWrite(pins::buzzer, HIGH);
    delay(70);
    digitalWrite(pins::buzzer, LOW);
    if (!configurationValid) {
        lastMessage = text("Brak poprawnej konfiguracji SD",
                           "Invalid or missing SD configuration",
                           "Configuración SD ausente o inválida",
                           "SD-Konfiguration fehlt oder ungültig");
    } else if (!rtcAvailable) {
        lastMessage = text("Brak zegara RTC", "RTC unavailable",
                           "RTC no disponible", "RTC nicht verfügbar");
    } else if (!rfidAvailable) {
        lastMessage = text("Błąd czytnika RFID — sprawdź połączenia",
                           "RFID reader error — check wiring",
                           "Error RFID — revise las conexiones",
                           "RFID-Fehler — Verkabelung prüfen");
    } else if (sdAvailable && !sdFilesInitialized) {
        lastMessage = text("Błąd plików SD — sprawdź kartę",
                           "SD files unavailable — check card",
                           "Error en archivos SD — revise tarjeta",
                           "SD-Dateien fehlen — Karte prüfen");
    } else if (configurationValid && config.mode == Mode::Offline &&
               !offlineEmployeesLoaded) {
        lastMessage = text("Brak poprawnego employees.csv",
                           "Missing or invalid employees.csv",
                           "employees.csv ausente o inválido",
                           "employees.csv fehlt oder ungültig");
    } else if (config.mode == Mode::Online && WiFi.status() != WL_CONNECTED) {
        lastMessage = text("Brak połączenia online — tryb bez przełączenia",
                           "Online unavailable — no offline fallback",
                           "Servicio en línea no disponible",
                           "Online-Dienst nicht verfügbar");
    } else {
        lastMessage = "";
    }
    screen = Screen::Home;
    drawHome();
    Serial.printf("RCP2027 ready: display=%s sd=%s rtc=%s rfid=%s employees=%u\n",
                  displayReady ? "ok" : "error", sdAvailable ? "ok" : "error",
                  rtcAvailable ? "ok" : "error", rfidAvailable ? "ok" : "error",
                  static_cast<unsigned>(employees.size()));
}

void loopRcp() {
    handleTouch();
    if (screen == Screen::Home || screen == Screen::AddEmployeeCard) handleCard();

    if (screen == Screen::AttendanceConfirmation &&
        static_cast<int32_t>(millis() - attendanceConfirmationUntil) >= 0) {
        screen = Screen::Home;
        attendanceConfirmationTitle = "";
        attendanceConfirmationName = "";
        lastMessage = "";
        lastClockDraw = 0;
        drawHome();
    }

    if (relayAutoClosed) {
        relayAutoClosed = false;
        relayOffAt = 0;
        if (relayWasActive) {
            relayWasActive = false;
            addLog("INFO", "RELAY_UNLOCK_PULSE_COMPLETED", "");
        }
    } else if (relayOffAt != 0 &&
               static_cast<int32_t>(millis() - relayOffAt) >= 0) {
        setRelay(false);
    }

    if (configurationValid && config.mode == Mode::Online &&
        static_cast<int32_t>(millis() - nextNetworkAttemptAt) >= 0) {
        nextNetworkAttemptAt = millis() + 10000;
        if (WiFi.status() != WL_CONNECTED) {
            onlineEmployeesLoaded = false;
            employees.clear();
            WiFi.reconnect();
        } else if (!onlineEmployeesLoaded || millis() >= nextEmployeeRefreshAt) {
            nextEmployeeRefreshAt = millis() + kEmployeeRefreshMs;
            if (!loadEmployeesFromOnlineSheet()) {
                Serial.println("Online employee refresh failed; no offline fallback");
            } else {
                lastMessage = "";
                if (screen == Screen::EmployeeList) drawEmployeeList();
                else if (screen == Screen::Home) drawHome();
            }
        }
    }

    if (screen == Screen::Home && millis() - lastClockDraw >= kClockRefreshMs) {
        lastClockDraw = millis();
        refreshHomeClock();
    }
    if ((screen == Screen::PinEntry || screen == Screen::PinChange ||
         screen == Screen::PinConfirm) &&
        millis() - screenSince > 30000) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pinInput = "";
        pendingPin = "";
        employeeEnrollmentRequested = false;
        pendingEmployeeUid = "";
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
    }
    if (screen == Screen::EmployeeEnroll && millis() - screenSince > 90000) {
        screen = employeeManagementReturnToList ? Screen::EmployeeList : Screen::Home;
        pendingEmployeeUid = "";
        lastMessage = text("Rejestrację anulowano", "Registration timed out",
                           "Registro cancelado", "Registrierung abgelaufen");
        if (employeeManagementReturnToList) drawEmployeeList();
        else drawHome();
    }
    delay(12);
}

}  // namespace

void setup() {
    setupRcp();
}

void loop() {
    loopRcp();
}
