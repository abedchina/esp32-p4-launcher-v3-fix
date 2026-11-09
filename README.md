# ESP32-P4 Launcher – תיקון ללוח Guition 4.3" עם שבב v3.2

## עדכון: יש גרסה 0.29 שעובדת
המורה תיקן את הבעיה בגרסה **0.29.0**, והיא נבדקה ועובדת על הלוח (מוצג chip v3).

| קובץ | גרסה | מתי להשתמש |
|---|---|---|
| `launcher_guition_p4_43_v3_0.29.0_full.bin` | **0.29** (רשמי של המורה) | **מומלץ** לצריבה |
| `launcher_guition_p4_43_v3_0.29.0_AbedMihilia_full.bin` | **0.29** + השם שלי | 0.29 עם "Abed Mihilia" במסך הבית |
| `launcher_guition_p4_43_v3_0.28.1_FIXED_full.bin` | **0.28** (התיקון שלי) | גיבוי בלבד |

מגרסה 0.29 אפשר להשתמש ב-**Settings → Update** בלי חשש (אבל בגרסה עם השם, עדכון ימחק את השם).

`patch_name.py` מחליף את השורה "ESP32-P4  Application Hub" במסך הבית בטקסט אחר ומתקן checksum ו-SHA256:
`python patch_name.py <in.bin> <out.bin> "Abed Mihilia"`

## הבעיה
הקושחה `launcher_guition_p4_43_v3_full.bin` של המורה (גרסאות 0.27.1 ו-0.28.1) קורסת על לוח
**Guition JC4880P443C_I_W V2.3** עם שבב **ESP32-P4 v3.2**: המסך נשאר שחור והלוח מתחיל מחדש כל 2 שניות.

הסיבה: בהגדרות של המסך מקור השעון הוא `MIPI_DSI_PHY_CLK_SRC_DEFAULT`, ערך שמתאים רק לשבבים הישנים (v1).
בשבב v3 הערך הזה גורם ל-`abort()` בתוך `esp_lcd_new_dsi_bus`.
דיווח למורה: https://github.com/giltal/ESP32_P4_Launcher/issues/15

## התיקון
בקובץ הבינארי שונה ערך אחד בלבד: מקור השעון של ה-DSI PHY עבר מ-6 (`PLL_F20M`) ל-18 (`XTAL`)
(הפקודה `c.li a5,6` הוחלפה ב-`c.li a5,18` בכתובת `0x40021116`). אחר כך חושבו מחדש ה-checksum וה-SHA256 של הקובץ.
`patch.py` הוא הסקריפט שביצע את התיקון.

## צריבה
1. פותחים https://espressif.github.io/esptool-js/ (Chrome או Edge) ולוחצים **Connect**
2. Flash Address: **0x0**, ובוחרים את הקובץ `launcher_guition_p4_43_v3_0.29.0_full.bin` (או את 0.28 המתוקן)
3. לוחצים **Program**, ובסוף **RESET**

בגרסה 0.28 המתוקנת לא לעשות Settings → Update. מגרסה 0.29 זה כבר בטוח.

---
Original project: https://github.com/giltal/ESP32_P4_Launcher (see `LICENSE-original`). Binary patch only: DSI PHY PLL reference clock 6 (PLL_F20M) → 18 (XTAL).

## כרטיס SD
התיקייה `SDcard` כוללת כבר את כל 23 האפליקציות (בתוך `apps`), את קבצי המשחקים החינמיים ואת קושחת שבב ה-Wi-Fi (`update/c6.bin`). אין צורך ב-Store.
1. מפרמטים את הכרטיס ל-**FAT32**
2. מעתיקים את **התוכן** של `SDcard` לשורש הכרטיס, כך שבכרטיס יופיעו ישר `apps`, `config`, `roms` וכו'
3. מכניסים את הכרטיס ללוח ולוחצים RESET. האפליקציות מופיעות במסך הבית.
