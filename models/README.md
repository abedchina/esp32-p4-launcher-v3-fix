# מודלים לאפליקציית AI Models (ESP32-P4)

כל מודל = **שני קבצים** עם אותו שם:

| קובץ | מה זה |
|---|---|
| `שם.espdl` | המודל עצמו (int8, לפורמט ESP-DL של ה-P4) |
| `שם.txt` | תיאור: שם, סוג, תוויות, סף |

דוגמה ל-`hand_robot.txt`:
```
name=Hand Robot
task=yolo
labels=forward,left,right,stop
conf=0.3
scale=0.0039215686
offset=0
```

## איך מוסיפים מודל חדש
1. בדף הזה: **Add file → Upload files**.
2. גוררים את שני הקבצים (`.espdl` + `.txt`) → **Commit changes**.
3. בלוח: **AI Models → Download models from GitHub** – מודלים חדשים יורדים לכרטיס (`/sd/models`), מודלים שכבר קיימים בכרטיס לא יורדים שוב.

המרת מודל YOLO (best.pt / tflite) ל-`.espdl`: Claude עושה את זה (tools/ai בענף launcher-source).
