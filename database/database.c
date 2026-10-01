#include "database.h"
#include <stdio.h>
#include <string.h>

sqlite3 *db;

int execute_query(const char *query) {
	sqlite3_stmt *stmt;

	int rc = sqlite3_prepare_v2(db, query, -1, &stmt, 0);
	printf("%s\n", query);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return SQLITE_ERROR;
	}

	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return SQLITE_OK;
}

sqlite3_stmt * execute_single_row_request(const char *query) {
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare(db, query, -1, &stmt, 0);
	
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	
	rc = sqlite3_step(stmt);
	
	if (rc != SQLITE_ROW) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	return stmt;
}

sqlite3_stmt * execute_multirow_request(const char *query) {
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare(db, query, -1, &stmt, 0);
	
	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	
	return stmt;
}

int db_new_lv_family(const char *family_name) {
	char query[500];
	sprintf(query, "INSERT INTO LauncherFamily (Name) "
				   "VALUES ('%s');", family_name);
	if(execute_query(query) != SQLITE_OK) return -1;
	return (int) sqlite3_last_insert_rowid(db);
}

int db_new_lv(const char *lv_name, int family_id, double payload_diameter, double A, double c_d,
				  double leo, double sso, double gto, double tli, double tvi) {
	char query[500];
	sprintf(query, "INSERT INTO LV (Name, FamilyID, PayloadDiameter, A, c_d, LEO, SSO, GTO, TLI, TVI) "
				   "VALUES ('%s', %d, %f, %f, %f, %f, %f, %f, %f, %f);",
				   lv_name, family_id, payload_diameter, A, c_d, leo, sso, gto, tli, tvi);
	if(execute_query(query) != SQLITE_OK) return -1;
	return (int) sqlite3_last_insert_rowid(db);
}

struct PlaneInfo_DB db_get_plane_from_id(int id) {
	char query[48];
	sprintf(query, "SELECT * FROM Plane WHERE PlaneID = %d", id);
	sqlite3_stmt *stmt;
	struct PlaneInfo_DB fv = {-1};

	int rc = sqlite3_prepare(db, query, -1, &stmt, 0);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return fv;
	}

	rc = sqlite3_step(stmt);

	if (rc != SQLITE_ROW) {
		fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		return fv;
	}

	for (int i = 0; i < sqlite3_column_count(stmt); i++) {
		const char *columnName = sqlite3_column_name(stmt, i);

		if 			(strcmp(columnName, "PlaneID") == 0) {
			fv.id = sqlite3_column_int(stmt, i);
		} else if 	(strcmp(columnName, "Name") == 0) {
			sprintf(fv.name, (char *) sqlite3_column_text(stmt, i));
		} else if 	(strcmp(columnName, "Status") == 0) {
			int status = sqlite3_column_int(stmt, i);
			fv.status = status == 1 ? ACTIVE : INACTIVE;
		} else if 	(strcmp(columnName, "RatedAltitude") == 0) {
			fv.rated_alt = sqlite3_column_int(stmt, i);
		} else if 	(strcmp(columnName, "F_vac") == 0) {
			fv.F_vac = sqlite3_column_double(stmt, i);
		} else if 	(strcmp(columnName, "F_sl") == 0) {
			fv.F_sl = sqlite3_column_double(stmt, i);
		} else if 	(strcmp(columnName, "Burnrate") == 0) {
			fv.burnrate = sqlite3_column_double(stmt, i);
		} else if 	(strcmp(columnName, "Mass_wet") == 0) {
			fv.mass_wet = sqlite3_column_double(stmt, i);
		} else if 	(strcmp(columnName, "Mass_dry") == 0) {
			fv.mass_dry = sqlite3_column_double(stmt, i);
		} else {
			//fprintf(stderr, "!!!!!! %s not yet implemented for Plane !!!!!\n", columnName);
		}
	}
	sqlite3_finalize(stmt);

	return fv;
}

int db_get_id_of_last_inserted_row() {
	return (int) sqlite3_last_insert_rowid(db);
}

int init_db() {
	FILE *existing_db = fopen("DERA.db", "rb");
	if(existing_db == NULL) {
		FILE *template_db = fopen("../Database/template.db", "rb");
		if(template_db == NULL) {
			fprintf(stderr, "Cannot open database template: ../Database/template.db\n");
			return SQLITE_CANTOPEN;
		}
		FILE *new_db = fopen("DERA.db", "wb");
		if(new_db == NULL) {
			fprintf(stderr, "Cannot create database: DERA.db\n");
			fclose(template_db);
			return SQLITE_CANTOPEN;
		}

		char buffer[8192];
		size_t bytes_read;
		int copy_succeeded = 1;
		while((bytes_read = fread(buffer, 1, sizeof(buffer), template_db)) > 0) {
			if(fwrite(buffer, 1, bytes_read, new_db) != bytes_read) {
				copy_succeeded = 0;
				break;
			}
		}
		if(ferror(template_db)) copy_succeeded = 0;
		fclose(template_db);
		if(fclose(new_db) != 0) copy_succeeded = 0;
		if(!copy_succeeded) {
			fprintf(stderr, "Failed to initialize database from template.\n");
			remove("DERA.db");
			return SQLITE_IOERR;
		}
	} else {
		fclose(existing_db);
	}

	int rc = sqlite3_open("DERA.db", &db);
	if(rc != SQLITE_OK) {
		fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
		sqlite3_close(db);
		db = NULL;
		return rc;
	}
	return SQLITE_OK;
}

void close_db() {
	sqlite3_close(db);
}