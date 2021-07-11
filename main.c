#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <locale.h>
#include <wchar.h>
#include <regex.h>

#define BUFFER_SIZE 257
// backtrack needed because what may look like a departure position may be destination position
#define BACKTRACK_SIZE 8
#define ERROR_MESSAGE_SIZE 256
#define DARK_TILE_COLOR 130
#define LIGHT_TILE_COLOR 223
#define WHITE_PIECE_COLOR 250
#define BLACK_PIECE_COLOR 0

#define UNICODE_BLACK_CHESS_PAWN 0x265f
#define UNICODE_SPACE 0x0020

#define WHITE_CAN_CASTLE_KINGSIDE 1
#define WHITE_CAN_CASTLE_QUEENSIDE 2
#define BLACK_CAN_CASTLE_KINGSIDE 4
#define BLACK_CAN_CASTLE_QUEENSIDE 8

#define MAX_TAG_NAME_SIZE 256
#define MAX_TAG_VALUE_SIZE 256

#define ERROR_EOF 1
#define ERROR_UNEXPECTED_CHARACTER 2

#define EMPTY_ROW { empty, empty, empty, empty, empty, empty, empty, empty }
#define MATCHED(p,i) (p[i].rm_so != (size_t)-1 && p[i].rm_so != p[i].rm_eo)

typedef enum {symbolToken = 1, fullMoveToken = 2, openTagToken = 3, closeTagToken = 4, quotedStringToken = 5, probabilityToken = 6} tokenType;
typedef enum {white, black} playerSide;
typedef enum {pawn=1, knight=2, bishop=3, rook=4, queen=5, king=6} pieceEnum;
typedef enum {empty = 0, blackPawn = -1, blackKnight = -2, blackBishop = -3, blackRook = -4, blackQueen = -5, blackKing = -6, whitePawn = 1, whiteKnight = 2, whiteBishop = 3, whiteRook = 4, whiteQueen = 5, whiteKing = 6} sidedPiece;
typedef enum {aFile = 1, bFile = 2, cFile = 3, dFile = 4, eFile = 5, fFile = 6, gFile = 7, hFile = 8} chessFile;

void print_board(sidedPiece board[8][8], bool aswhite) {
    for (int rank = aswhite ? 7 : 0; aswhite ? rank >= 0 : rank < 8; aswhite ? rank-- : rank++) {
        for (int file = aswhite ? 0 : 7; aswhite ? file <= 7 : file >= 0; aswhite ? file++ : file--) {
            sidedPiece sp = board[rank][file];
            pieceEnum p = sp > 0 ? sp : -sp;
            wchar_t unicodePoint = UNICODE_BLACK_CHESS_PAWN - p + 1;
            if (p == 0) {
                unicodePoint = UNICODE_SPACE;
            }

            int tileColor = (rank+file) % 2 == 0 ? DARK_TILE_COLOR : LIGHT_TILE_COLOR;
            int pieceColor = sp > 0 ? WHITE_PIECE_COLOR : BLACK_PIECE_COLOR;

            // set foreground color, set background color and print unicode point
            // only half of the chess piece appears unless there is a space after it
            wprintf(L"\e[38;5;%dm\e[48;5;%dm %lc ", pieceColor, tileColor, unicodePoint);
        }
        // reset colors and newline
        wprintf(L"\e[0m\n");
    }
    wprintf(L"\n");
}

typedef struct {
    chessFile file;
    int rank;
} position;

typedef struct moveTag {
    position departurePosition;
    pieceEnum piece;
    playerSide side;
    sidedPiece sidedPiece;
    pieceEnum promoteTo;
    position destination;
    bool isCapture;
    bool isCheck;
    bool isCheckmate;
    bool isShortCastling;
    bool isLongCastling;
} move;

typedef struct moveTreeTag {
    move* move;
    int decisionLevel;
    int probability;
    bool isRoot;
    int fullMoveNo;
    int halfMoveNo; // from start of game for backtracking, not draw clock
    struct moveTreeTag* firstChoice;
    struct moveTreeTag* nextChoice;
    struct moveTreeTag* previousMove;
} moveTree;

move* new_move() {
    move* m = (move*)malloc(sizeof(move));
    if (m == NULL) {
        fprintf(stderr, "failed to allocate memory for new move\n");
        exit(1);
    }
    m->departurePosition.file = 0;
    m->departurePosition.rank = 0;
    m->destination.file = 0;
    m->destination.rank = 0;
    m->promoteTo = pawn;
    m->isCapture = m->isCheck = m->isCheckmate = m->isShortCastling = m->isLongCastling = 0;
    m->side = black; // fake root node is black in order to switch to white for first move
    return m;
}

moveTree* new_move_tree() {
    moveTree* t = (moveTree*)malloc(sizeof(moveTree));
    if (t == NULL) {
        fprintf(stderr, "failed to allocate memory for new move tree\n");
        exit(1);
    }
    t->firstChoice = NULL;
    t->nextChoice = NULL;
    t->previousMove = NULL;
    t->isRoot = t->decisionLevel = t->probability = t->fullMoveNo = t->halfMoveNo = 0;
    t->move = new_move();
    return t;
}

void append_move(moveTree* previous, moveTree* next) {
    next->previousMove = previous;
    if (!previous->firstChoice) {
        previous->firstChoice = next;
    } else {
        moveTree* lastChoice = previous->firstChoice;
        while (lastChoice->nextChoice != NULL) {
            lastChoice = lastChoice->nextChoice;
        }
        lastChoice->nextChoice = next;
    }
}

/** Create a new board having the initial chess position. */
void init_board(sidedPiece board[8][8]) {
    sidedPiece board_array[8][8] = {
        { whiteRook, whiteKnight, whiteBishop, whiteQueen, whiteKing, whiteBishop, whiteKnight, whiteRook },
        { whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn, whitePawn },
        EMPTY_ROW,
        EMPTY_ROW,
        EMPTY_ROW,
        EMPTY_ROW,
        { blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn, blackPawn },
        { blackRook, blackKnight, blackBishop, blackQueen, blackKing, blackBishop, blackKnight, blackRook }
    };
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) {
            board[i][j] = board_array[i][j];
        }
    }
}

typedef struct {
    sidedPiece board[8][8];
    playerSide sidePlaying;
    position* enPassantTarget;
    int castlingAvailability;
    int halfMoveClock;
    int fullMoveNo;
} gameState;

gameState* new_game() {
    gameState* game = (gameState*)malloc(sizeof(gameState));
    if (game == NULL) {
        fprintf(stderr, "Failed to allocate memory for game state.\n");
        exit(1);
    }

    init_board(game->board);

    game->sidePlaying = white;
    game->enPassantTarget = NULL;
    game->castlingAvailability = 0;
    game->halfMoveClock = 0;
    game->fullMoveNo = 1;

    return game;
}

typedef struct {
    FILE* file;
    int line;
    int column;
    char buffer[BUFFER_SIZE];
    int bufferSize;
    int bufferCursor; // offset from start of buffer that we have already read
    char errorMessage[ERROR_MESSAGE_SIZE];
    int decisionLevel;
    moveTree* moveTreeTip;
    moveTree* moveTreeRoot;
    int totalCharacterCount;
    gameState* initGameState;
} parser;

parser make_parser(FILE* file) {
    parser p;
    p.file = file;
    p.line = 1;
    p.column = 1;
    p.moveTreeRoot = p.moveTreeTip = new_move_tree();
    p.moveTreeTip->move->side = black;
    p.moveTreeTip->isRoot = true;
    p.moveTreeTip->fullMoveNo = 0;
    p.moveTreeTip->halfMoveNo = 0;
    p.decisionLevel = 0;
    p.totalCharacterCount = 0;
    p.initGameState = new_game();
    return p;
}

typedef struct {
    bool hasError;
    union {
        char errorMessage[ERROR_MESSAGE_SIZE];
        parser* parser;
        move* move;
    };
} parseResult;

parseResult make_parse_error(parser* p, char* msg) {
    parseResult res;
    res.hasError = true;
    strncpy(res.errorMessage, msg, ERROR_MESSAGE_SIZE);
    sprintf(res.errorMessage, "parser error: %s at line %d, column %d", msg, p->line, p->column);
    return res;
}

parseResult make_parse_parser_result(parser* p) {
    parseResult res;
    res.hasError = false;
    res.parser = p;
    return res;
}

parseResult make_parse_move_result(move* m) {
    parseResult res;
    res.hasError = false;
    res.move = m;
    return res;
}

gameState* parse_fen(char* record) {
    gameState* game = new_game();
    char c;
    int file, rank, offset = 0;
    int numEmptySquares;

    // board state
    char* token = strtok(record, " ");
    for (rank = 7; rank >= 0; --rank) {
        file = 0;
        while (token[offset] != 0) {
            c = token[offset++];
            if (c == '/') {
                break;
            }
            switch (c) {
                case 'p':
                    game->board[rank][file++] = blackPawn;
                    break;
                case 'n':
                    game->board[rank][file++] = blackKnight;
                    break;
                case 'b':
                    game->board[rank][file++] = blackBishop;
                    break;
                case 'r':
                    game->board[rank][file++] = blackRook;
                    break;
                case 'q':
                    game->board[rank][file++] = blackQueen;
                    break;
                case 'k':
                    game->board[rank][file++] = blackKing;
                    break;
                case 'P':
                    game->board[rank][file++] = whitePawn;
                    break;
                case 'N':
                    game->board[rank][file++] = whiteKnight;
                    break;
                case 'B':
                    game->board[rank][file++] = whiteBishop;
                    break;
                case 'R':
                    game->board[rank][file++] = whiteRook;
                    break;
                case 'Q':
                    game->board[rank][file++] = whiteQueen;
                    break;
                case 'K':
                    game->board[rank][file++] = whiteKing;
                    break;
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                    numEmptySquares = c - '0';
                    for (int i = 0; i < numEmptySquares; ++i) {
                        game->board[rank][file++] = empty;
                    }
                    break;
            }
        }
    }

    // playing side
    token = strtok(NULL, " ");
    if (strcmp(token, "w") == 0) {
        game->sidePlaying = white;
    } else if (strcmp(token, "b") == 0) {
        game->sidePlaying = black;
    } else {
        fprintf(stderr, "failed to read playing side %s\n", token);
        return NULL;
    }

    // castling availability
    token = strtok(record, " ");
    for (int i = 0; i < strlen(token); ++i) {
        char c = token[i];
        switch (c) {
            case 'K':
                game->castlingAvailability |= WHITE_CAN_CASTLE_KINGSIDE;
                break;
            case 'Q':
                game->castlingAvailability |= WHITE_CAN_CASTLE_QUEENSIDE;
                break;
            case 'k':
                game->castlingAvailability |= BLACK_CAN_CASTLE_KINGSIDE;
                break;
            case 'q':
                game->castlingAvailability |= BLACK_CAN_CASTLE_QUEENSIDE;
                break;
            default:
                // TODO: invalid character
                break;
        }
    }

    // parse algebraic notation position**(not move) or "-"
    token = strtok(record, " "); // TODO

    token = strtok(record, " ");
    game->halfMoveClock = atoi(token);

    token = strtok(record, " ");
    game->fullMoveNo = atoi(token);

    return game;
}


typedef struct {
    bool hasError;
    tokenType tokenType;
    char token[256];
    int number;
    playerSide side;
    char errorMessage[256];
    bool terminated;
    bool eol;
} lexResult;

char* lexBuffer = NULL;
lexResult lexical_analysis(char* buffer) {
    lexResult res;
    res.number = res.hasError = 0;
    if (buffer != NULL) {
        lexBuffer = buffer;
    }
    // ignore whitespace
    while (*lexBuffer == ' ' || *lexBuffer == '\t' || *lexBuffer == '\n') {
        lexBuffer = lexBuffer + 1;
    }
    if (*lexBuffer == 0) {
        res.eol = true;
        return res;
    }
    res.eol = false;
    char c = *lexBuffer;
    int i = 0;
    if (c == '[') {
        res.tokenType = openTagToken;
        lexBuffer += 1;
        return res;
    } else if (c == ']') {
        res.tokenType = closeTagToken;
        lexBuffer += 1;
        return res;
    } else if (c == '"') {
        res.tokenType = quotedStringToken;
        i += 1;
        while (true) {
            if (i >= strlen(lexBuffer)) {
                res.hasError = true;
                sprintf(res.errorMessage, "Unterminated quoted string.");
                return res;
            }
            c = lexBuffer[i];
            if (c == '"') {
                res.token[i-1] = 0;
                i++;
                break;
            }
            res.token[i-1] = c;
            i++;
        }
        lexBuffer += i;
        return res;
    }

    while (i < strlen(lexBuffer) && lexBuffer[i] != ' ' && lexBuffer[i] != '\t' && lexBuffer[i] != '\n') {
        res.token[i] = lexBuffer[i];
        i++;
    }
    res.token[i] = 0;
    if (strlen(res.token) == 0) {
        res.eol = true;
        return res;
    }
    lexBuffer += strlen(res.token);
    if (strlen(res.token) > 1 && res.token[strlen(res.token)-1] == '.') {
        res.tokenType = fullMoveToken;
        res.number = atoi(res.token);
        res.side = res.token[strlen(res.token)-2] == '.' ? black : white;
        return res;
    } else if (strlen(res.token) > 1 && res.token[strlen(res.token)-1] == '%') {
        res.tokenType = probabilityToken;
        res.number = atoi(res.token);
        return res;
    }
    // else
    res.tokenType = symbolToken;
    return res;
}

move* parse_algebraic_notation2(move* m, char* buffer) {
    if (strcmp(buffer, "O-O-O") == 0) {
        m->isLongCastling = true;
        m->piece = king;
        return m;
    } else if (strcmp(buffer, "O-O") == 0) {
        m->isShortCastling = true;
        m->piece = king;
        return m;
    }
    int destinationIndex = -1;
    for (int i = strlen(buffer)-1; i >= 0; i--) {
        if (buffer[i] >= 'a' && buffer[i] <= 'h') {
            destinationIndex = i;
            break;
        }
    }
    if (destinationIndex == -1) {
        // fprintf(stderr, "missing destination\n");
        return NULL;
    }
    int i = 0;
    // map offset from letter 'B' => piece enum, -1 for invalid pieces
    int8_t pieceLookup[] = {3, -1, -1, -1, -1, -1, -1, -1, -1, 6, -1, -1, 2, -1, 1, 5, 4};
    if (buffer[i] >= 'B' && buffer[i] <= 'R') {
        m->piece = pieceLookup[buffer[i++] - 'B'];
        if (m->piece == -1) {
            // fprintf(stderr, "invalid piece\n");
            return NULL;
        }
    } else {
        m->piece = pawn;
    }
    if (buffer[i] >= 'a' && buffer[i] <= 'h' && i < destinationIndex) {
        m->departurePosition.file = buffer[i++] - 'a' + 1;
    }
    if (buffer[i] >= '0' && buffer[i] <= '9' && i < destinationIndex) {
        m->departurePosition.rank = buffer[i++] - '1' + 1;
    }
    if (buffer[i] == 'x') {
        m->isCapture = true;
        i++;
    }
    if (buffer[i] >= 'a' && buffer[i] <= 'h' && i == destinationIndex) {
        m->destination.file = buffer[i++] - 'a' + 1;
    }
    if (buffer[i] >= '0' && buffer[i] <= '9' && i == destinationIndex+1) {
        m->destination.rank = buffer[i++] - '1' + 1;
    }
    if (buffer[i] == '=' && (buffer[i+1] >= 'B' && buffer[i+1] <= 'R')) {
        m->promoteTo = pieceLookup[buffer[i+1] - 'B'];
        if (m->promoteTo <= 0) {
            // fprintf(stderr, "invalid promotion\n");
            return NULL;
        }
        i += 2;
    }
    if (buffer[i] == '#') {
        m->isCheckmate = true;
        i++;
    } else if (buffer[i] == '+') {
        m->isCheck = true;
        i++;
    }
    if (i != strlen(buffer)) {
        // fprintf(stderr, "not fully parsed %d vs %ld\n", i, strlen(buffer));
        return NULL;
    }
    return m;
}

parseResult simple_parse(parser* p) {
    char buffer[BUFFER_SIZE];
    bool readTags = true;
    char tagName[BUFFER_SIZE];
    char errorMessage[256];
    move* m;
    int state = 0;
    int startLine = 1;
    // parse tags
    char* buf = fgets(buffer, BUFFER_SIZE, p->file);
    p->moveTreeTip->move->side = black; // initialize
    lexResult res;
    while (true) {
        // fprintf(stderr, "STATE: %d\n", state);
        if (startLine == 1) {
            res = lexical_analysis(buffer);
        } else {
            res = lexical_analysis(NULL);
        }

        startLine = 0;
        if (res.eol) {
            p->line++;
            char* buf = fgets(buffer, BUFFER_SIZE, p->file);
            // fprintf(stderr, "got new buffer:%s\n", buf);
            if (buf == NULL && state == 10) {
                break;
            }
            else if (buf == NULL) {
                return make_parse_error(p, "Unexpected end of file.");
            }
            if (strlen(buffer) > 255) {
                return make_parse_error(p, "Lines must be less than 255 characters.");
            }
            startLine = 1;
            continue;
        } else if (res.hasError) {
            return make_parse_error(p, res.errorMessage);
        }

        if ((state == 0 || state == 1) && res.tokenType != openTagToken) {
            state = 10;
        } else if (state == 0 || state == 1) {
            state = 2;
            continue;
        }

        if (state == 2) {
            if (res.tokenType != symbolToken) {
                sprintf(errorMessage, "Expected tag name, got %d", res.tokenType);
                return make_parse_error(p, errorMessage);
            }
            strcpy(tagName, res.token);
            state = 3;
            continue;
        }

        if (state == 3) {
            if (res.tokenType != quotedStringToken) {
                sprintf(errorMessage, "Expected tag value, got %s", res.token);
                return make_parse_error(p, errorMessage);
            }
            p->initGameState = parse_fen(res.token);
            if (p->initGameState == NULL) {
                return make_parse_error(p, "Invalid FEN\n");
            }
            p->moveTreeRoot->move->side = p->initGameState->sidePlaying == white ? black : white;
            state = 4;
            continue;
        }
        
        if (state == 4) {
            if (res.tokenType != closeTagToken) {
                sprintf(errorMessage, "Expected tag close, got %d", res.tokenType);
                return make_parse_error(p, errorMessage);
            }
            state = 1;
            continue;
        }

        if (state == 10 && res.tokenType != fullMoveToken) {
            state = 11;
        } else if (state == 10) {
            int targetHalfMoveNo = 2*(res.number-1)+1;
            if (res.side == black) {
                targetHalfMoveNo += 1;
            }
            if (p->moveTreeTip->isRoot) {
                // first move number dictates how moves are counted
                p->moveTreeTip->halfMoveNo = targetHalfMoveNo - 1;
                continue;
            } else if (targetHalfMoveNo == p->moveTreeTip->halfMoveNo + 1) {
                continue;
            } else if (targetHalfMoveNo > p->moveTreeTip->halfMoveNo + 1) {
                sprintf(errorMessage, "Wrong move number, skipped moves. %d vs %d\n", targetHalfMoveNo, p->moveTreeTip->halfMoveNo);
                return make_parse_error(p, errorMessage);
            }
            // else backtrack to that move
            // fprintf(stderr, "moving %d -> %d", p->moveTreeTip->halfMoveNo, targetHalfMoveNo - 1);
            while (p->moveTreeTip->halfMoveNo > targetHalfMoveNo - 1) {
                p->moveTreeTip = p->moveTreeTip->previousMove;
            }
            state = 11;
            continue;
        }
        if (state == 11 && res.tokenType != probabilityToken) {
            p->moveTreeTip->probability = 100;
            state = 12;
        } else if (state == 11) {
            p->moveTreeTip->probability = res.number;
            state = 12;
            continue;
        }
        if (state == 12) {
            if (res.tokenType != symbolToken) {
                sprintf(errorMessage, "Unexpected algebraic notation move, got %s", res.token);
                return make_parse_error(p, errorMessage);
            }
            moveTree* t = new_move_tree();
            t->fullMoveNo = p->moveTreeTip->move->side == white ? p->moveTreeTip->fullMoveNo : p->moveTreeTip->fullMoveNo + 1;
            t->halfMoveNo = p->moveTreeTip->halfMoveNo + 1;
            t->move->side = p->moveTreeTip->move->side == white ? black : white;
            t->move = parse_algebraic_notation2(t->move, res.token);
            t->move->sidedPiece = t->move->side == white ? t->move->piece : -(t->move->piece);
            if (t->move == NULL) {
                sprintf(errorMessage, "Not a valid algebraic notation move: %s", res.token);
                return make_parse_error(p, errorMessage);
            }
            append_move(p->moveTreeTip, t);
            p->moveTreeTip = t;
            state = 10;
            continue;
        }
    }

    return make_parse_parser_result(p);
}

void print_position(position pos) {
    switch (pos.file) {
        case aFile:
            wprintf(L"a");
            break;
        case bFile:
            wprintf(L"b");
            break;
        case cFile:
            wprintf(L"c");
            break;
        case dFile:
            wprintf(L"d");
            break;
        case eFile:
            wprintf(L"e");
            break;
        case fFile:
            wprintf(L"f");
            break;
        case gFile:
            wprintf(L"g");
            break;
        case hFile:
            wprintf(L"h");
            break;
    }
    if (pos.rank) {
        wprintf(L"%d", pos.rank);
    }
}

void print_piece(pieceEnum p) {
    switch (p) {
        case pawn:
            break;
        case knight:
            wprintf(L"N");
            break;
        case bishop:
            wprintf(L"B");
            break;
        case rook:
            wprintf(L"R");
            break;
        case queen:
            wprintf(L"Q");
            break;
        case king:
            wprintf(L"K");
            break;
        default:
            fprintf(stderr, "Unexpected piece in move: %d\n", p);
            exit(1);
    }
}

void print_algebraic_notation(move* m) {
    if (m->isShortCastling) {
        wprintf(L"O-O");
        return;
    }
    if (m->isLongCastling) {
        wprintf(L"O-O-O");
        return;
    }
    print_piece(m->piece);
    if (m->departurePosition.file || m->departurePosition.rank) {
        print_position(m->departurePosition);
    }
    if (m->isCapture) {
        wprintf(L"x");
    }
    print_position(m->destination);
    if (m->promoteTo != pawn) {
        wprintf(L"=");
        print_piece(m->promoteTo);
    }
    if (m->isCheck) {
        wprintf(L"+");
    }
    if (m->isCheckmate) {
        wprintf(L"#");
    }
}

void print_tree(moveTree* m) {
    // TODO fix because decision level is obsolete
    if (m->previousMove && m->previousMove->decisionLevel != m->decisionLevel) {
        wprintf(L"\n");
        for (int i = 0; i < m->decisionLevel; ++i) {
            wprintf(L"\t");
        }
    }
    if (m->probability != 0) {
        wprintf(L"%d%% ", m->probability);
    }
    print_algebraic_notation(m->move);
    wprintf(L" ");
    moveTree*c = m->firstChoice;
    while (c != NULL) {
        wprintf(L"\n");
        print_tree(c);
        c = c->nextChoice;
    }
}

/* Returning a random double floating point from 0 to 1. */
double random_probability() {
    return (double)rand() / (double)RAND_MAX;
}

/** Decide which move to use from the movement tree. Selects moves according to their probability weight. */
moveTree* choose_move(moveTree* currentMove) {
    double totalProbabilityWeight = 0;
    moveTree* choice = currentMove->firstChoice;
    while (choice != NULL) {
        totalProbabilityWeight += choice->probability;
        choice = choice->nextChoice;
    }

    double targetWeight = random_probability() * totalProbabilityWeight;
    double currentWeight = 0;
    choice = currentMove->firstChoice;
    while (choice != NULL) {
        currentWeight += choice->probability;
        if (currentWeight > targetWeight) {
            return choice;
        }
        choice = choice->nextChoice;
    }

    return choice;
}

/** compare two moves, disregarding child/sibling/parent choices in the tree, and probabilities */
bool moves_equal(move* m1, move* m2) {
    return m1->departurePosition.rank == m2->departurePosition.rank && m1->departurePosition.file == m2->departurePosition.file && m1->piece == m2->piece && m1->destination.rank == m2->destination.rank && m1->destination.file == m2->destination.file;
}

/** Add move to move tree. */
moveTree* tree_apply_move(moveTree* t, move* newMove) {
    moveTree* c = t->firstChoice;
    while (c != NULL) {
        if (moves_equal(c->move, newMove)) {
            return c;
        }
        c = c->nextChoice;
    }
    return NULL;
}

typedef struct potentialMoveTag {
    int rankBy;
    int fileBy;
    struct potentialMoveTag *nextPotentialMove;
} potentialMove;

/** Add a potential move to the list. */
potentialMove* add_potential_move(potentialMove* list, int rankBy, int fileBy) {
    potentialMove* p = (potentialMove*)malloc(sizeof(potentialMove));
    if (p == NULL) {
        fprintf(stderr, "not enough memory for potential move");
        exit(1);
    }

    p->rankBy = rankBy;
    p->fileBy = fileBy;
    p->nextPotentialMove = list;

    return p;
}

/** Free memory allocated for potential move list. */
void free_potential_move(potentialMove* list) {
    if (list == NULL) {
        return;
    }
    free_potential_move(list->nextPotentialMove);
    free(list);
}

/** Check if a move from departure (rank, file) to destination (rank, file) would require jumping any pieces, irrespective of moving piece. */
bool no_pieces_jumped(sidedPiece board[8][8], int fromRank, int fromFile, int toRank, int toFile) {
    int rankStep = toRank > fromRank ? 1 : toRank < fromRank ? -1 : 0;
    int fileStep = toFile > fromFile ? 1 : toFile < fromFile ? -1 : 0;
    int currentRank = fromRank + rankStep;
    int currentFile = fromFile + fileStep;
    while (currentRank != toRank || currentFile != toFile) {
        if (board[currentRank][currentFile] != empty) {
            return false;
        }
        currentRank = currentRank + rankStep;
        currentFile = currentFile + fileStep;
    }
    return true;
}

/*
 * Apply the move m to the board, returning true if the move was unique and legal.
 *
 * Disambiguates moves such as Re1 that doesn't specify which rook moves to e1.
 * Assumes that the notation already uniquely specifies piece.
 * TODO: fail and warn when move not unique or illegal.
 */
bool board_apply_move(sidedPiece board[8][8], move* m) {
    if (m->isShortCastling) {
        if (m->side == white) {
            board[0][4] = empty;
            board[0][7] = empty;
            board[0][5] = whiteRook;
            board[0][6] = whiteKing;
        } else {
            board[7][4] = empty;
            board[7][7] = empty;
            board[7][5] = blackRook;
            board[7][6] = blackKing;
        }
        return true;
    } else if (m->isLongCastling) {
        if (m->side == white) {
            board[0][4] = empty;
            board[0][0] = empty;
            board[0][3] = whiteRook;
            board[0][2] = whiteKing;
        } else {
            board[7][4] = empty;
            board[7][0] = empty;
            board[7][3] = blackRook;
            board[7][2] = blackKing;
        }
        return true;
    }
    potentialMove* potentialMoveTip = NULL;
    switch (m->piece) {
        case pawn:
            if (m->isCapture) {
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, 1);
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, -1);
            } else {
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 1 : -1, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, m->side == white ? 2 : -2, 0);
            }
            break;
        case knight:
            potentialMoveTip = add_potential_move(potentialMoveTip, 2, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -2, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 2, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -2, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 2);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 2);
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, -2);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, -2);
            break;
        case bishop:
            for (int i = 1; i < 8; ++i) {
                // right diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, -i);
                // left diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, -i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, i);
            }
            break;
        case rook:
            for (int i = 1; i < 8; ++i) {
                // same rank
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, -i);
                // same file
                potentialMoveTip = add_potential_move(potentialMoveTip, i, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, 0);
            }
            break;
        case queen:
            for (int i = -7; i < 8; ++i) {
                // same rank
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, 0, -i);
                // same file
                potentialMoveTip = add_potential_move(potentialMoveTip, i, 0);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, 0);
                // right diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, -i);
                // left diagonal
                potentialMoveTip = add_potential_move(potentialMoveTip, i, -i);
                potentialMoveTip = add_potential_move(potentialMoveTip, -i, i);
            }
            break;
        case king:
            // same rank
            potentialMoveTip = add_potential_move(potentialMoveTip, 0, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, 0, -1);
            // same file
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 0);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 0);
            // right diagonal
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, 1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, -1);
            // left diagonal
            potentialMoveTip = add_potential_move(potentialMoveTip, 1, -1);
            potentialMoveTip = add_potential_move(potentialMoveTip, -1, 1);
            break;
    }
    int fromRank = 0, fromFile = 0;
    bool found = false;
    while (potentialMoveTip != NULL) {
        fromRank = m->destination.rank-1 - potentialMoveTip->rankBy;
        fromFile = m->destination.file-1 - potentialMoveTip->fileBy;
        // fprintf(stderr, "checking %d %d (piece=%d)\n", fromFile, fromRank, m->piece);
        if (fromRank >= 0 && fromFile >= 0 && fromRank < 8 && fromFile < 8 &&
            (m->departurePosition.rank == 0 || m->departurePosition.rank == fromRank+1) &&
            (m->departurePosition.file == 0 || m->departurePosition.file == fromFile+1)) {
            sidedPiece sp = board[fromRank][fromFile];
            playerSide s = sp > 0 ? white : black;
            pieceEnum p = sp > 0 ? sp : -sp;
            if (s == m->side && p == m->piece) {
                // cannot go to position occupied by same colored piece
                if (board[m->destination.rank-1][m->destination.file-1] * sp <= 0) {
                    if (p == knight || no_pieces_jumped(board, fromRank, fromFile, m->destination.rank-1, m->destination.file-1)) {
                        found = true;
                        break;
                    }
                }
            }
        }
        potentialMoveTip = potentialMoveTip->nextPotentialMove;
    }
    free_potential_move(potentialMoveTip);
    if (!found) {
        fprintf(stderr, "illegal move!\n");
        exit(1);
    }
    // wprintf(L"from rank %d file %d to rank %d file %d\n", fromRank, fromFile, m->destination.rank-1, m->destination.file-1);
    board[fromRank][fromFile] = empty;
    board[m->destination.rank-1][m->destination.file-1] = m->sidedPiece;
    return true;
}

/** Choose random element from an array of pointers. */
void* random_array_choice(void** choices, int numChoices) {
    int choiceNum = (int)floor(random_probability() * numChoices);
    return choices[choiceNum];
}

void print_greeting() {
    char* greetings[4] = {"Let's play chess!", "Good luck, have fun!", "Let's go!", "Let's see if you  know how to play this opening."};
    char* s = (char*)random_array_choice((void**)greetings, sizeof(greetings)/sizeof(char*));
    wprintf(L"%s\n", s);
}

void print_goodbye() {
    char* messages[2] = {"Goodbye!", "See you again soon!"};
    char* s = (char*)random_array_choice((void**)messages, sizeof(messages)/sizeof(char*));
    wprintf(L"%s\n", s);
}

void print_do_not_understand() {
    char* messages[4] = {"Sorry, I did not understand.", "That doesn't look like a move nor a command.", "Sorry, please rephrase.", "Are you sure that's a move (or command)?"};
    char* s = (char*)random_array_choice((void**)messages, sizeof(messages)/sizeof(char*));
    wprintf(L"%s\n", s);
}

void play(moveTree* tree, gameState* game, bool blindMode) {
    char* buffer = (char*)malloc(BUFFER_SIZE*sizeof(char));
    //print_board(theBoard.board);
    print_greeting();
    setvbuf(stdin, NULL, _IOLBF, -1);
    moveTree* moveTreeTip = tree;
    moveTree* moveTreeRoot = tree;
    bool viewAsWhite = moveTreeRoot->move->side == black;
    if (!blindMode) {
        wprintf(L"\n");
        print_board(game->board, viewAsWhite);
    }
    while (moveTreeTip != NULL) {
        // printf("currentMove:\n");
        if (!moveTreeTip->isRoot) {
            board_apply_move(game->board, moveTreeTip->move);
            print_algebraic_notation(moveTreeTip->move);
            wprintf(L"\n");
            if (!blindMode) {
                print_board(game->board, viewAsWhite);
            }
        }
        if (moveTreeTip->firstChoice == NULL) {
            break;
        }
        // printf("\n");
        while (true) {
            wprintf(L"> ");
            buffer = fgets(buffer, BUFFER_SIZE, stdin);
            if (buffer == NULL || strcmp(buffer, "exit\n") == 0) {
                if (buffer == NULL) {
                    wprintf(L"ctl-d\n");
                }
                print_goodbye();
                exit(0);
            }

            buffer[strcspn(buffer, "\n")] = 0;

            move*m = new_move();
            m = parse_algebraic_notation2(m, buffer);
            if (m == NULL) {
                //fprintf(stderr, "Failed to parse: %s\n", res.errorMessage);
                print_do_not_understand();
                continue;
            }

            // printf("got move:\n");
            // print_algebraic_notation(res.moveTreeRoot);
            moveTree* goToMove = tree_apply_move(moveTreeTip, m);
            if (goToMove == NULL) {
                wprintf(L"wrong move! try again:\n");
            } else {
                moveTreeTip = goToMove;
                board_apply_move(game->board, moveTreeTip->move);
                if (!blindMode) {
                    print_board(game->board, viewAsWhite);
                }
                break;
            }
        }
        moveTreeTip = choose_move(moveTreeTip);
    }
    wprintf(L"Line played correctly. Good job!\n");
    free(buffer);
}

typedef struct {
    char* inputPath;
    bool asBlack;
    bool asWhite;
    bool blindMode;
} options;

options init_options() {
    options options;
    options.inputPath = "";
    options.asBlack = false;
    options.asWhite = false;
    options.blindMode = false;
    return options;
}

options parse_options(int argc, char* argv[]) {
    options options = init_options();
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--black") == 0) {
            options.asBlack = true;
        } else if (strcmp(argv[i], "--white") == 0) {
            options.asWhite = true;
        } else if (strcmp(argv[i], "--blind") == 0) {
            options.blindMode = true;
        } else if (strlen(options.inputPath) == 0) {
            options.inputPath = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Invalid option %s.\n", argv[i]);
        } else {
            fprintf(stderr, "Unexpected multiple arguments.\n");
        }
    }
    return options;
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");
    srand(time(0));

    if (argc < 2) {
        fprintf(stderr, "No variants input file specified.\nUsage: $ %s INPUT_FILE\n", argv[0]);
        exit(1);
    }

    options options = parse_options(argc, argv);

    FILE* fp = fopen(options.inputPath, "r");

    parser p = make_parser(fp);
    parseResult res = simple_parse(&p);
    if (res.hasError) {
        fprintf(stderr, "%s", res.errorMessage);
        return 1;
    }
    print_tree(p.moveTreeRoot->firstChoice);
    if (options.asWhite && p.moveTreeRoot->move->side != black || options.asBlack && p.moveTreeRoot->move->side != white) {
        // let computer play first move if tree starts from the other side than user selected
        res.parser->moveTreeRoot = choose_move(res.parser->moveTreeRoot);
    }
    play(res.parser->moveTreeRoot, res.parser->initGameState, options.blindMode);
}
