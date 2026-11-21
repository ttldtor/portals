# Orders notebook

This viewer displays **Markdown source**.
The document stays editable outside the viewer.

## Review checklist

- Read the SQL schema on the left.
- Scroll each document independently.
- Use `Tab` to move keyboard focus.
- Press F10 to close the application.

## Query example

```mysql
SELECT city, SUM(amount) AS total
FROM orders
WHERE confirmed = TRUE
GROUP BY city;
```

## Source, not rendered HTML

Fences, headings and code have semantic colors.
The file is read in bounded windows on workers.
A cold seek shows plain text before exact color.

[Portals documentation](https://ttldtor.github.io/portals/)

## Closing note

The application does not load the whole file.
The selected theme supplies the syntax palette.
