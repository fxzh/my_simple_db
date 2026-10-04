CREATE TABLE t_LC (ID int, Name varchar(16));
INSERT INTO T_LC (NAME, ID) VALUES ('b', 2);
SELECT Id, name FROM t_LC WHERE NAME = 'b';
SELECT * FROM "t_LC";
CREATE TABLE "t_Mixed" (id int);
SELECT * FROM t_Mixed;
SELECT * FROM "t_Mixed";
DROP TABLE "t_Mixed";
DROP TABLE T_LC;
