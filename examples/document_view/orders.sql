-- Orders: a readable MySQL source example.
CREATE TABLE orders (
  id BIGINT PRIMARY KEY,
  city VARCHAR(80),
  amount DECIMAL(10, 2),
  confirmed BOOLEAN DEFAULT FALSE
);

/* A multiline comment retains its color
   when the viewer opens in the middle. */
INSERT INTO orders VALUES
  (1, 'Prague', 129.50, TRUE),
  (2, 'Bratislava', 82.00, FALSE),
  (3, 'Oslo', 319.25, TRUE);

SELECT city, SUM(amount) AS total
FROM orders
WHERE confirmed = TRUE
GROUP BY city
ORDER BY total DESC;

DELIMITER $$
CREATE PROCEDURE order_total(IN requested_id BIGINT)
BEGIN
  SELECT amount FROM orders WHERE id = requested_id;
END$$
DELIMITER ;

-- End of the example.
