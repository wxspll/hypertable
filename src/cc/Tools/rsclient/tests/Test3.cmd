UPDATE Test3 "Test2-data.txt";
CREATE SCANNER ON Test3[..??];
DESTROY SCANNER;
CREATE SCANNER ON Test3[..??] MAX_VERSIONS=2;
DESTROY SCANNER;
quit
