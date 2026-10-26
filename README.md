# MilagrosNote

Editor de notas de voz, **gratuito y de código abierto** (VST3), para FL Studio y otros DAWs en PC.
Ves tu voz como bloques de notas (como en Melodyne) y la corriges con un clic hacia la nota
que pide la tonalidad y los acordes del beat.

> **Versión 0.1, aún sin probar en PC.** Es la primera versión: puede tener errores.
> Nada de esto funciona como magia: mover una nota muchos semitonos suena artificial.

## Qué hace

- **Capturar notas:** reproduces (o cantas) tu voz una sola vez y el plugin dibuja tus notas.
- **Corrección total (1 clic):** lleva cada nota a la nota correcta según tu tonalidad
  y, si hay acordes del beat, prefiere las notas del acorde.
- **Mover una nota con un clic:** selecciona un bloque y pulsa Subir 1, Bajar 1,
  "Nota que pide el beat" u Original.
- **Acordes del beat:** detecta acordes mayores y menores y los muestra en una franja
  sobre la pantalla de notas. Es una aproximación: con 808 muy fuertes o acordes
  complejos puede equivocarse.
- **Estabilizar:** cuánto se aplana el vibrato dentro de cada nota. **Transición:** qué
  tan suave es el paso entre notas.
- **Escucharme afinado al cantar:** mientras capturas, corrige en vivo hacia la escala.
- **Asesor (hoja de consejos):** rango de tu voz, tonalidad de tu voz y del beat, qué
  porcentaje de notas ya está afinado, y avisos. Con botones para aplicar.
- Las notas y correcciones se **guardan en el proyecto** de FL Studio.

## Cómo usarlo (voz)

1. Pon MilagrosNote en el canal de tu voz (después de MilagrosLimpio).
2. Elige la tonalidad (o usa el Asesor / "Usar tonalidad del beat").
3. Pulsa **Capturar notas** y reproduce el tema (o canta) de principio a fin, una vez.
4. Pulsa **Detener captura**: aparecen los bloques de notas.
5. Pulsa **Corrección total**, o haz clic en una nota y muévela.
6. Reproduce el tema: la corrección se aplica sobre tu voz.

## Acordes del beat (dos formas)

1. **Sidechain:** envía el beat al sidechain del plugin (si tu versión de FL Studio lo permite).
2. **Segunda copia:** pon otra copia de MilagrosNote en el canal del beat, cámbiala al
   modo **Instrumental** y reproduce el tema. Esa copia analiza los acordes y se los pasa
   a la copia de la voz (funciona si las dos están en el mismo FL Studio).

Si ninguna funciona, elige la tonalidad a mano.

## Importante

- El plugin usa la **posición del tema** que informa el DAW. Si tu voz no está en el
  mismo lugar al capturar y al reproducir, la corrección no caerá donde debe.
  Captura reproduciendo la voz grabada en tu playlist.
- Latencia aproximada: 12-14 ms (FL Studio la compensa).

## Límites conocidos

- Voces muy ruidosas o susurradas: la detección falla (usa MilagrosLimpio antes).
- Mover notas más de 2 o 3 semitonos suena artificial (no corrige formantes).
- Los acordes son una aproximación (solo mayores y menores).
- Límite de captura: unos 13 minutos.

## Ideas para contribuir

- Corrector de formantes, acordes de séptima, detección de ritmo (BPM)
- Dibujar y mover notas con el mouse, dividir y unir notas
- Mejor reducción de errores de octava

## Cómo compilarlo

Súbelo a GitHub y se compila solo (`.github/workflows/compilar.yml`). Al terminar, descarga
**MilagrosNote-Windows** desde Actions > Artifacts y ejecuta `INSTALAR.bat` como administrador.
Para una versión descargable en "Releases", crea una etiqueta (por ejemplo `v0.1.0`). Licencia: **AGPL-3.0** (usa JUCE).
